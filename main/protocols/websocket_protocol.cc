#include "websocket_protocol.h"
#include "board.h"
#include "system_info.h"
#include "application.h"
#include "settings.h"

#include <cstring>
#include <cJSON.h>
#include <esp_log.h>
#include <arpa/inet.h>
#include "assets/lang_config.h"

#define TAG "WS"

/*
 * WebSocket 实现将控制面和音频数据复用在一条可靠、有序的连接上：文本帧是 JSON，二进制
 * 帧是 Opus。它省去了独立 UDP/AES 参数协商，但音频也会受到 TCP 重传和队头阻塞影响。
 * 协议版本 2/3 带不同包头，旧版本直接发送裸 Opus 以保持服务端兼容性。
 */

WebsocketProtocol::WebsocketProtocol() {
    // EventGroup 用于把异步收到的 server hello 同步给正在等待的打开通道流程。
    event_group_handle_ = xEventGroupCreate();
}

WebsocketProtocol::~WebsocketProtocol() {
    // 显式释放握手事件组；析构函数体结束后 websocket_ 会随 unique_ptr 成员自动释放。
    vEventGroupDelete(event_group_handle_);
}

bool WebsocketProtocol::Start() {
    /*
     * WebSocket 不保留空闲控制连接，因此 Start() 只表示协议已准备好。真正的 DNS/TLS/WS
     * 握手延迟发生在用户唤醒后的 OpenAudioChannel()。
     */
    return true;
}

bool WebsocketProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    /*
     * 根据协商/配置版本序列化二进制帧：
     * - v2：version/type/reserved/timestamp/payload_size + Opus；
     * - v3：type/reserved/payload_size + Opus；
     * - 其他版本：仅发送 Opus payload。
     * 所有整数在网络上传输前转换为大端序。
     */
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    if (version_ == 2) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol2) + packet->payload.size());
        auto bp2 = (BinaryProtocol2*)serialized.data();
        bp2->version = htons(version_);
        bp2->type = 0;
        bp2->reserved = 0;
        bp2->timestamp = htonl(packet->timestamp);
        bp2->payload_size = htonl(packet->payload.size());
        memcpy(bp2->payload, packet->payload.data(), packet->payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else if (version_ == 3) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol3) + packet->payload.size());
        auto bp3 = (BinaryProtocol3*)serialized.data();
        bp3->type = 0;
        bp3->reserved = 0;
        bp3->payload_size = htons(packet->payload.size());
        memcpy(bp3->payload, packet->payload.data(), packet->payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else {
        return websocket_->Send(packet->payload.data(), packet->payload.size(), true);
    }
}

bool WebsocketProtocol::SendText(const std::string& text) {
    // 文本帧仅承载 JSON 控制消息；发送失败会使当前音频通道立即失效。
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    if (!websocket_->Send(text)) {
        ESP_LOGE(TAG, "Failed to send text: %s", text.c_str());
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }

    return true;
}

bool WebsocketProtocol::IsAudioChannelOpened() const {
    // 同时检查对象、底层连接、协议错误和下行超时，排除 TCP 半开连接。
    return websocket_ != nullptr && websocket_->IsConnected() && !error_occurred_ && !IsTimeout();
}

void WebsocketProtocol::CloseAudioChannel(bool send_goodbye) {
    /* WebSocket 的连接生命周期就是音频会话生命周期，断开本身等价于 goodbye。 */
    (void)send_goodbye;
    websocket_.reset();
}

bool WebsocketProtocol::OpenAudioChannel() {
    /*
     * 打开顺序：读取 OTA 下发配置 -> 创建 WebSocket -> 设置认证与设备身份请求头 -> 注册收包/
     * 断线回调 -> 建立连接 -> 发送 client hello -> 等待最多 10 秒 server hello -> 通知上层。
     * 任一步失败都返回 false，Application 会停留或回退到可恢复状态。
     */
    Settings settings("websocket", false);
    std::string url = settings.GetString("url");
    std::string token = settings.GetString("token");
    int version = settings.GetInt("version");
    if (version != 0) {
        version_ = version;
    }

    // 新会话清除上一条连接留下的错误门闩。
    error_occurred_ = false;

    auto network = Board::GetInstance().GetNetwork();
    websocket_ = network->CreateWebSocket(1);
    if (websocket_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create websocket");
        return false;
    }

    /*
     * HTTP Upgrade 握手通过 Authorization 验证客户端；Protocol-Version 选择二进制帧格式，
     * Device-Id 和 Client-Id 分别标识硬件与持久化客户端实例。
     */
    if (!token.empty()) {
        // 允许配置只保存 token 本体；未包含认证方案时自动补 Bearer 前缀。
        if (token.find(" ") == std::string::npos) {
            token = "Bearer " + token;
        }
        websocket_->SetHeader("Authorization", token.c_str());
    }
    websocket_->SetHeader("Protocol-Version", std::to_string(version_).c_str());
    websocket_->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    websocket_->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());

    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        /*
         * OnData 运行在 WebSocket 网络回调上下文。二进制帧转换为拥有独立 payload 的
         * AudioStreamPacket；文本 JSON 只在回调期间有效，Application 如需异步使用会复制字段。
         */
        if (binary) {
            // 按版本拆包，统一转换为 AudioStreamPacket，隐藏各版本头部差异。
            if (on_incoming_audio_ != nullptr) {
                if (version_ == 2) {
                    // v2 保留服务端时间戳，可用于播放时序或 AEC 对齐。
                    BinaryProtocol2* bp2 = (BinaryProtocol2*)data;
                    bp2->version = ntohs(bp2->version);
                    bp2->type = ntohs(bp2->type);
                    bp2->timestamp = ntohl(bp2->timestamp);
                    bp2->payload_size = ntohl(bp2->payload_size);
                    auto payload = (uint8_t*)bp2->payload;
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = bp2->timestamp,
                        .payload = std::vector<uint8_t>(payload, payload + bp2->payload_size)
                    }));
                } else if (version_ == 3) {
                    // v3 精简头不含时间戳，上层统一填 0。
                    BinaryProtocol3* bp3 = (BinaryProtocol3*)data;
                    bp3->payload_size = ntohs(bp3->payload_size);
                    auto payload = (uint8_t*)bp3->payload;
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>(payload, payload + bp3->payload_size)
                    }));
                } else {
                    // v1/兼容模式把整帧都视为 Opus 数据。
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>((uint8_t*)data, (uint8_t*)data + len)
                    }));
                }
            }
        } else {
            // hello 属于传输协商由本类消费；tts/stt/llm/mcp 等业务 JSON 交给 Application。
            auto root = cJSON_ParseWithLength(data, len);
            auto type = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(type)) {
                if (strcmp(type->valuestring, "hello") == 0) {
                    ParseServerHello(root);
                } else {
                    if (on_incoming_json_ != nullptr) {
                        on_incoming_json_(root);
                    }
                }
            } else {
                ESP_LOGE(TAG, "Missing message type, data: %s", std::string(data, len).c_str());
            }
            cJSON_Delete(root);
        }
        // 任意有效方向的服务端数据都刷新活跃时间，供 IsTimeout() 检测半开连接。
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    websocket_->OnDisconnected([this]() {
        // 断线等价于音频通道关闭，由 Application 恢复 Idle 和低功耗策略。
        ESP_LOGI(TAG, "Websocket disconnected");
        if (on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
    });

    ESP_LOGI(TAG, "Connecting to websocket server: %s with version: %d", url.c_str(), version_);
    if (!websocket_->Connect(url.c_str())) {
        ESP_LOGE(TAG, "Failed to connect to websocket server, code=%d", websocket_->GetLastError());
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    // TLS/WebSocket 握手只建立传输连接，发送 hello 后才开始业务协议参数协商。
    auto message = GetHelloMessage();
    if (!SendText(message)) {
        return false;
    }

    // 等待服务端 hello，确保采样率、帧长和 session_id 已协商完成。
    EventBits_t bits = xEventGroupWaitBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }

    return true;
}

std::string WebsocketProtocol::GetHelloMessage() {
    /*
     * Client hello 声明当前二进制协议版本、WebSocket 传输、MCP/AEC 能力和上行 Opus 参数。
     * 服务端据此创建 session，并在响应里给出下行采样率与帧时长。
     */
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", version_);
    cJSON* features = cJSON_CreateObject();
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);
    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}

void WebsocketProtocol::ParseServerHello(const cJSON* root) {
    /*
     * 只接受 transport=websocket 的 hello，保存本轮 session_id 和服务端音频参数。最后置位
     * HELLO 事件，表示 OpenAudioChannel() 可以安全通知 Application 开始发送/接收音频。
     */
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (transport == nullptr || strcmp(transport->valuestring, "websocket") != 0) {
        ESP_LOGE(TAG, "Unsupported transport: %s", transport->valuestring);
        return;
    }

    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
        ESP_LOGI(TAG, "Session ID: %s", session_id_.c_str());
    }

    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio_params)) {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (cJSON_IsNumber(sample_rate)) {
            server_sample_rate_ = sample_rate->valueint;
        }
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (cJSON_IsNumber(frame_duration)) {
            server_frame_duration_ = frame_duration->valueint;
        }
    }

    xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
}
