#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <cJSON.h>
#include <string>
#include <functional>
#include <chrono>
#include <vector>

/**
 * 协议层与 AudioService 之间的统一压缩音频包。
 * payload 通常是一帧 Opus；采样率和帧长描述解码参数，timestamp 用于播放排序或 AEC 对齐。
 * 使用 unique_ptr 在任务/回调间转移所有权，避免复制音频负载。
 */
struct AudioStreamPacket {
    int sample_rate = 0;       // PCM 解码目标采样率，单位 Hz
    int frame_duration = 0;    // 单个 Opus 包代表的音频时长，单位 ms
    uint32_t timestamp = 0;    // 协议提供的毫秒时间戳；不支持时为 0
    std::vector<uint8_t> payload;  // Opus 压缩数据
};

// WebSocket 协议 v2 二进制帧头。多字节字段发送时使用网络字节序。
struct BinaryProtocol2 {
    uint16_t version;       // 当前为 2
    uint16_t type;          // 消息类型：0 为 Opus，1 为 JSON
    uint32_t reserved;      // 保留字段，便于协议扩展
    uint32_t timestamp;     // 毫秒时间戳，供服务端 AEC 对齐
    uint32_t payload_size;  // 负载字节数
    uint8_t payload[];      // 变长负载
} __attribute__((packed));

// WebSocket 协议 v3 精简帧头；控制 JSON 已使用文本帧，因此二进制帧无需携带时间戳和版本。
struct BinaryProtocol3 {
    uint8_t type;           // 0 表示 Opus 音频
    uint8_t reserved;       // 保留字段
    uint16_t payload_size;  // 网络字节序负载长度
    uint8_t payload[];      // 变长 Opus 数据
} __attribute__((packed));

// 主动中止服务端 TTS 时携带原因，服务端可据此区分普通取消与唤醒词打断。
enum AbortReason {
    kAbortReasonNone,
    kAbortReasonWakeWordDetected
};

// 监听模式决定服务端何时结束一轮输入，以及设备是否允许边播边录。
enum ListeningMode {
    kListeningModeAutoStop,   // 服务端根据 VAD 自动判断用户说完
    kListeningModeManualStop, // 一直采集，直到设备显式发送 stop
    kListeningModeRealtime    // 全双工实时采集，需要设备端或服务端 AEC
};

/**
 * 云端语音协议的统一抽象。
 *
 * Application 只面向该接口完成会话控制：Start() 启动控制连接，OpenAudioChannel() 发起
 * hello 协商，SendAudio()/SendText() 传输数据，CloseAudioChannel() 结束本轮会话。
 * MqttProtocol 使用 MQTT+UDP，WebsocketProtocol 使用单条 WebSocket，但对上层暴露相同语义。
 *
 * 派生类的网络回调可能运行在协议组件任务中；这里不强制切换线程。Application 注册的回调
 * 若要修改状态机或 UI，必须通过事件位或 Schedule() 回到主任务。
 */
class Protocol {
public:
    virtual ~Protocol() = default;

    // 以下参数由服务端 hello 协商，AudioService 据此配置下行 Opus 解码。
    inline int server_sample_rate() const {
        return server_sample_rate_;
    }
    inline int server_frame_duration() const {
        return server_frame_duration_;
    }
    inline const std::string& session_id() const {
        return session_id_;
    }

    // 注册生命周期和收包回调；每种回调只保留最后一次注册值。
    void OnIncomingAudio(std::function<void(std::unique_ptr<AudioStreamPacket> packet)> callback);
    void OnIncomingJson(std::function<void(const cJSON* root)> callback);
    void OnAudioChannelOpened(std::function<void()> callback);
    void OnAudioChannelClosed(std::function<void()> callback);
    void OnNetworkError(std::function<void(const std::string& message)> callback);
    void OnConnected(std::function<void()> callback);
    void OnDisconnected(std::function<void()> callback);

    // 启动长期控制连接或完成按需连接前的初始化。
    virtual bool Start() = 0;
    // 打开一轮实时音频会话，并等待服务端 hello 完成参数协商。
    virtual bool OpenAudioChannel() = 0;
    // 关闭本轮音频会话；send_goodbye=false 用于响应服务端主动关闭，避免重复回包。
    virtual void CloseAudioChannel(bool send_goodbye = true) = 0;
    // 连接存在、未发生错误且最近仍有下行数据时返回 true。
    virtual bool IsAudioChannelOpened() const = 0;
    // 接管音频包所有权并发送；失败返回 false，让 Application 停止排空发送队列。
    virtual bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) = 0;

    // 与具体传输无关的 JSON 会话控制消息。
    virtual void SendWakeWordDetected(const std::string& wake_word);
    virtual void SendStartListening(ListeningMode mode);
    virtual void SendStopListening();
    virtual void SendAbortSpeaking(AbortReason reason);
    virtual void SendMcpMessage(const std::string& message);

protected:
    // 具体协议在网络回调中触发这些函数；Application 负责必要的主任务切换。
    std::function<void(const cJSON* root)> on_incoming_json_;
    std::function<void(std::unique_ptr<AudioStreamPacket> packet)> on_incoming_audio_;
    std::function<void()> on_audio_channel_opened_;
    std::function<void()> on_audio_channel_closed_;
    std::function<void(const std::string& message)> on_network_error_;
    std::function<void()> on_connected_;
    std::function<void()> on_disconnected_;

    int server_sample_rate_ = 24000;    // 服务端下行音频采样率
    int server_frame_duration_ = 60;    // 服务端单帧时长
    bool error_occurred_ = false;       // 一旦置位，当前通道立即视为不可用
    std::string session_id_;            // hello 创建的会话 ID，后续控制消息必须携带
    std::chrono::time_point<std::chrono::steady_clock> last_incoming_time_; // 最近下行活动时间

    // 所有控制消息最终都收敛到实现层的文本发送接口。
    virtual bool SendText(const std::string& text) = 0;
    // 错误只上报一次当前事实，不直接销毁派生类连接。
    virtual void SetError(const std::string& message);
    // 使用单调时钟判断 120 秒无任何下行数据的半开连接。
    virtual bool IsTimeout() const;
};

#endif // PROTOCOL_H

