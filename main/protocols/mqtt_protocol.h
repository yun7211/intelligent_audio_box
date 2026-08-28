#ifndef MQTT_PROTOCOL_H
#define MQTT_PROTOCOL_H


#include "protocol.h"
#include <mqtt.h>
#include <udp.h>
#include <cJSON.h>
#include <mbedtls/aes.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>

#include <functional>
#include <string>
#include <map>
#include <mutex>
#include <memory>
#include <atomic>

#define MQTT_PING_INTERVAL_SECONDS 90
#define MQTT_RECONNECT_INTERVAL_MS 60000

#define MQTT_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

/**
 * MQTT 控制面 + AES-CTR UDP 音频数据面协议。
 *
 * 生命周期：Start() 建立长期 MQTT 连接；OpenAudioChannel() 通过 MQTT 发送 hello，等待服务端
 * 返回 UDP 地址、AES 密钥、nonce 和音频参数；随后创建 UDP socket 收发 Opus；关闭会话时只
 * 销毁 UDP，MQTT 继续保留用于下一轮会话和自动重连。
 *
 * channel_mutex_ 保护 UDP 句柄与发送路径，MQTT 回调负责 JSON 控制消息，UDP 回调负责实时
 * 音频。两条数据路径最终都通过 Protocol 回调汇入 Application。
 */
class MqttProtocol : public Protocol {
public:
    MqttProtocol();
    ~MqttProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;

private:
    // 延迟回调共享的存活标记；析构时先置 false，防止已投递任务访问悬空的 this。
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    
    // OpenAudioChannel() 阻塞等待 MQTT 回调解析完 server hello。
    EventGroupHandle_t event_group_handle_;

    // 所有客户端 JSON 控制消息发布到该主题；订阅关系由底层 MQTT 配置建立。
    std::string publish_topic_;

    std::mutex channel_mutex_;
    // MQTT 承载 JSON 控制面，UDP 只在音频会话期间存在并承载加密 Opus 数据面。
    std::unique_ptr<Mqtt> mqtt_;
    std::unique_ptr<Udp> udp_;
    // hello 下发的 AES-128 会话参数；CTR 加解密共用同一 key 和每包 nonce。
    mbedtls_aes_context aes_ctx_;
    std::string aes_nonce_;
    std::string udp_server_;
    int udp_port_;
    // 上下行独立递增序号。上行写入 nonce；下行用于丢弃旧包并诊断 UDP 丢包。
    uint32_t local_sequence_;
    uint32_t remote_sequence_;
    // MQTT 断线后延迟重连，避免服务器不可用时高频握手。
    esp_timer_handle_t reconnect_timer_;

    // 创建 MQTT 客户端、注册回调并连接；report_error 控制是否向 UI 报告即时建连失败。
    bool StartMqttClient(bool report_error=false);
    // 解析 hello 中的 session、音频参数和 UDP/AES 配置，并唤醒等待任务。
    void ParseServerHello(const cJSON* root);
    // 把服务端十六进制 key/nonce 转为原始字节串。
    std::string DecodeHexString(const std::string& hex_string);

    // MQTT 发布 JSON 控制消息。
    bool SendText(const std::string& text) override;
    // 声明 transport=udp、Opus 参数及 MCP/AEC 能力。
    std::string GetHelloMessage();
};


#endif // MQTT_PROTOCOL_H
