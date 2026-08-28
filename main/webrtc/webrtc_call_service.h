#ifndef WEBRTC_CALL_SERVICE_H
#define WEBRTC_CALL_SERVICE_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

// 独立的 WebRTC 音视频通话，与语音助手的对话链路互不依赖。
// 由 CONFIG_USE_WEBRTC_CALL 门控，默认关闭。
//
// 状态机，由 esp_webrtc 的事件驱动（见 OnWebrtcEvent）：
//
//   kIdle --Start()--> kStarting --（引擎就绪）--> kWaiting
//     kWaiting --CONNECTING/PAIRED--> kConnecting --CONNECTED--> kInCall
//     任意状态 --Stop() | CONNECT_FAILED | DISCONNECTED--> kStopping --> kIdle
//
// kConnecting 覆盖的是"对端出现了"到"媒体真正流动"之间那段窗口：esp_webrtc 在
// ICE 收集候选时报 CONNECTING、SDP 匹配后报 PAIRED，但只在 CONNECTED 才开始推流。
// 把它与 kWaiting 分开的意义在于：**"没人接"** 和 **"接了但连不上"** 从此在状态和
// 日志上是两回事 —— 这两种故障的排查方向完全相反（前者查用户/网页，后者查网络/NAT）。
enum class WebrtcCallState { kIdle, kStarting, kWaiting, kConnecting, kInCall, kStopping };

// 上一通电话是怎么结束的。esp_webrtc 把 CONNECT_FAILED（ICE 从未建立）和
// DISCONNECTED（连上过，对端离开）作为两个独立事件上报；这里在应用层保持这个区分，
// 而不是笼统合并成一个"通话结束"。kSetupError 是本地失败：通话根本没走到网络这一步
// （拿不到 codec handle、媒体桥构建失败、或引擎启动失败）。
enum class WebrtcEndReason {
    kNone, kLocalHangup, kSetupError, kConnectFailed, kPeerDisconnected
};

// 一条对端控制消息**是从哪条通道进来的**。这个信息必须一路带到业务层：远程开门只认
// DataChannel（DTLS/SCTP，端到端加密且只在通话建立后存在），走信令通道来的同名命令
// 一律拒绝。用应用层自己的枚举而不是直接暴露 esp_webrtc_custom_data_via_t，是为了让
// 这个头文件保持不依赖 esp_webrtc（与 webrtc_handle_ 用 void* 的理由相同）。
enum class WebrtcDataVia { kUnknown, kSignaling, kDataChannel };

class WebrtcCallService {
public:
    static WebrtcCallService& GetInstance() {
        static WebrtcCallService instance;
        return instance;
    }

    bool Start();                 // 进入通话模式；返回 true 表示已受理
    void Stop();                  // 挂断，恢复助手
    std::string GetInfo();        // 连接 URL / 设备 IP（本地信令模式），均不含凭证
    std::string GetStats();       // 通话计时与结局计数，JSON 格式
    const char* GetStateName() const;
    WebrtcCallState state() const { return state_.load(); }
    WebrtcEndReason end_reason() const { return end_reason_.load(); }
    // 控制通道是否真的可写。SCTP 连上不等于具体通道已 OPENED，所以这是独立的一位。
    bool data_channel_ready() const { return data_channel_ready_.load(); }

    // AppRTC 信令的房间号（拼接到信令 URL 后面）。
    // 仅在外部 AppRTC 模式下使用，本地 HTTP 信令模式会忽略它 —— 那条路直接连设备 IP，
    // 没有房间的概念。
    void SetRoomId(const std::string& room_id) { room_id_ = room_id; }
    const std::string& room_id() const { return room_id_; }

    // 注册通话状态变化的回调。**该回调可能从 WebRTC 事件任务里触发**，所以处理函数
    // 如果要碰 UI，必须自己做线程跳转。把这个约束写进接口文档，而不是指望调用方猜。
    void SetStateChangeHandler(std::function<void(WebrtcCallState)> handler) {
        state_change_handler_ = std::move(handler);
    }

    // 以下三个由 WebRTC 事件回调调用（内部使用）。
    void NotifyConnecting();                    // ICE 收集中 / SDP 已匹配
    void NotifyConnected();                     // 媒体已开始流动
    void NotifyEnded(WebrtcEndReason reason);   // 建连失败，或对端离开

    // 门铃自定义命令。SendCustomData() 优先走 DataChannel，通道尚未就绪时回退到信令
    // 通道（RING 这类建连前就要发的通知靠它）。**敏感命令不要用它** —— 见下面那个
    // 只走 DataChannel 的版本。
    void SetCustomDataHandler(std::function<void(const std::string&, WebrtcDataVia)> handler);
    void SendCustomData(const std::string& data);
    // 只走 DataChannel、**没有信令回退**的发送。远程开门的应答走这条：给信令通道上的
    // 对端回执，等于承认了一条我们不打算信任的控制路径。返回 true 表示已交给引擎。
    bool SendCustomDataViaDataChannel(const std::string& data);
    void HandleCustomData(const std::string& data, WebrtcDataVia via);  // 由 webrtc 回调调用
    void SetDataChannelReady(bool ready);             // 由 DataChannel 生命周期事件调用

private:
    WebrtcCallService() = default;
    void Cleanup();                    // 释放媒体桥 + 恢复助手，状态置 Idle
    bool FailStart(const char* what);  // Start() 所有失败分支的统一出口；恒返回 false
    void SetState(WebrtcCallState state);  // 更新 state_ 并触发变化回调
    void ResetCallTimeline();          // 为新一次尝试重开统计窗口

    // 开机以来的毫秒数，截断到 32 位。下面所有时长都按**无符号差值**计算，因此只要
    // 单次通话短于约 49 天，跨越 32 位毫秒回绕后结果依然正确。0 表示"该阶段未到达"。
    static uint32_t NowMs();

    // state_ 与下面的统计量由 WebRTC 事件任务写、主任务和 MCP 任务读，所以必须是原子的。
    // 全部选 32 位：Xtensa 有 32 位原子指令（S32C1I 比较交换），编译器能生成 lock-free
    // 代码；换成 64 位则没有对应指令，GCC 会退化成调用 libatomic 的辅助函数（内部靠锁），
    // 既有在中断上下文附近用锁的隐患，又平白多一份代码体积。
    std::atomic<WebrtcCallState> state_{WebrtcCallState::kIdle};
    std::atomic<WebrtcEndReason> end_reason_{WebrtcEndReason::kNone};
    // SCTP 已连接不代表具体通道已经可写，只有收到 OPENED 事件后才置 true。
    std::atomic<bool> data_channel_ready_{false};

    // 单次通话的时间线打点，由 Start() 重置。
    std::atomic<uint32_t> t_start_{0};       // Start() 受理时刻
    std::atomic<uint32_t> t_paired_{0};      // 首个 CONNECTING/PAIRED 事件
    std::atomic<uint32_t> t_connected_{0};   // CONNECTED：媒体开始流动
    std::atomic<uint32_t> last_talk_ms_{0};  // 上一通已结束通话的通话时长

    // 会话级计数器（自开机累计，不持久化 —— 掉电即失）。
    std::atomic<uint32_t> n_calls_{0};
    std::atomic<uint32_t> n_connected_{0};
    std::atomic<uint32_t> n_connect_failed_{0};
    std::atomic<uint32_t> n_peer_disconnected_{0};

    void* webrtc_handle_ = nullptr;    // esp_webrtc_handle_t（不透明 void*），通话期间由本类持有
    bool camera_released_ = false;     // 为通话释放过板载摄像头则为 true（决定要不要 Reacquire）
    std::string room_id_;              // AppRTC 房间号（空 = 用默认 URL）
    std::string signal_url_;           // 构造出的信令 URL，生命周期必须覆盖整通电话
    std::function<void(const std::string&, WebrtcDataVia)> custom_data_handler_;
    std::function<void(WebrtcCallState)> state_change_handler_;
};

#endif // WEBRTC_CALL_SERVICE_H
