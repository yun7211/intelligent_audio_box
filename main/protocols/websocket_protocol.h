#ifndef _WEBSOCKET_PROTOCOL_H_
#define _WEBSOCKET_PROTOCOL_H_


#include "protocol.h"

#include <web_socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#define WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

/**
 * 单连接 WebSocket 协议。
 *
 * JSON 控制消息使用文本帧，Opus 音频使用二进制帧，两类数据共享同一条 WebSocket。与 MQTT
 * 实现不同，Start() 不预连接；每次 OpenAudioChannel() 都创建连接、发送 hello 并等待参数
 * 协商，CloseAudioChannel() 则直接销毁整条连接。
 *
 * version_ 控制二进制音频封装：旧版发送裸 Opus，v2 带时间戳的完整包头，v3 使用精简包头。
 */
class WebsocketProtocol : public Protocol {
public:
    WebsocketProtocol();
    ~WebsocketProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;

private:
    // 网络 OnData 回调解析 server hello 后置位，唤醒 OpenAudioChannel()。
    EventGroupHandle_t event_group_handle_;
    // 整条会话连接的唯一所有者；reset 会触发底层断连和资源释放。
    std::unique_ptr<WebSocket> websocket_;
    // 从 NVS 读取的协议版本，默认兼容裸 Opus 的 v1。
    int version_ = 1;

    // 解析 session_id 和服务端下行 Opus 参数，完成 hello 握手。
    void ParseServerHello(const cJSON* root);
    // 发送 WebSocket 文本帧，失败时通过 Protocol::SetError() 上报。
    bool SendText(const std::string& text) override;
    // 构造客户端能力与上行音频参数声明。
    std::string GetHelloMessage();
};

#endif
