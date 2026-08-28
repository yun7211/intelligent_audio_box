#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <mutex>
#include <atomic>
#include <deque>
#include <memory>
#include <functional>

#include "protocol.h"
#include "ota.h"
#include "audio_service.h"
#include "device_state.h"
#include "device_state_machine.h"

/*
 * 主任务事件位。
 * 网络、音频、定时器等回调可能运行在不同 FreeRTOS 任务中，它们不直接执行复杂业务，只置位
 * 或把闭包放进 main_tasks_。Run() 被唤醒后在单一任务中串行消费事件，从而保护状态机、
 * protocol_ 生命周期和绝大多数 UI 操作。
 *
 * EventGroup 只表示“某类工作待处理”，相同事件连续发生可能合并；需要逐项执行的数据必须
 * 另存队列，例如音频发送队列和 main_tasks_。
 */
#define MAIN_EVENT_SCHEDULE             (1 << 0)
#define MAIN_EVENT_SEND_AUDIO           (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED   (1 << 2)
#define MAIN_EVENT_VAD_CHANGE           (1 << 3)
#define MAIN_EVENT_ERROR                (1 << 4)
#define MAIN_EVENT_ACTIVATION_DONE      (1 << 5)
#define MAIN_EVENT_CLOCK_TICK           (1 << 6)
#define MAIN_EVENT_NETWORK_CONNECTED    (1 << 7)
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 8)
#define MAIN_EVENT_TOGGLE_CHAT          (1 << 9)
#define MAIN_EVENT_START_LISTENING      (1 << 10)
#define MAIN_EVENT_STOP_LISTENING       (1 << 11)
#define MAIN_EVENT_STATE_CHANGED        (1 << 12)
#define MAIN_EVENT_BARGE_IN_CONFIRMED   (1 << 13)


// 回声消除部署位置决定监听模式以及协议 hello 中声明的能力。
enum AecMode {
    kAecOff,           // 不做 AEC，默认说完自动停止采集
    kAecOnDeviceSide,  // ESP-SR AFE 在设备端消除扬声器回声
    kAecOnServerSide,  // 上传带参考关系的音频，由服务端处理回声
};

/**
 * 整机应用编排器。
 *
 * Application 不实现具体驱动或网络协议，而是连接 Board、AudioService、Protocol、Ota、
 * Display 和 DeviceStateMachine。其主要生命周期为：
 *
 * app_main -> Initialize -> Run
 *                         -> 网络连接
 *                         -> ActivationTask（资源/版本/激活/协议）
 *                         -> Idle
 *                         -> 唤醒或按键 -> Connecting -> Listening <-> Speaking
 *
 * 除显式注明的原子变量外，业务状态应在 Run() 所在主任务中修改。
 */
class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // 全局唯一的应用编排器，不允许复制。
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /**
     * 初始化显示、音频、状态监听器和网络回调。
     * 网络连接异步启动，连接结果通过主任务事件返回。
     */
    void Initialize();

    /**
     * 运行主事件循环。该函数在主任务中执行且不会返回，集中处理网络、状态变化、
     * 用户交互以及其他任务通过 Schedule() 投递的工作。
     */
    void Run();

    // 状态机内部使用原子值，因此其他任务可安全读取当前状态。
    DeviceState GetDeviceState() const { return state_machine_.GetState(); }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    
    /**
     * 请求状态转换；只有符合 DeviceStateMachine 规则时才会成功。
     */
    bool SetDeviceState(DeviceState state);

    /**
     * 将回调投递到主任务执行，是跨任务修改业务状态或 UI 的统一入口。
     */
    void Schedule(std::function<void()>&& callback);

    /**
     * 同步更新状态、消息、表情，并可选播放提示音。
     */
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();

    void AbortSpeaking(AbortReason reason);

    /**
     * 为 WebRTC 独占音频链路而挂起助手对话：关闭协议音频通道、中止正在播放的 TTS、
     * 将状态机拉回 Idle，并在 ResumeConversation() 前阻止任何新对话。
     */
    void SuspendConversation();

    /**
     * 解除对话挂起，使设备重新接受唤醒或云端对话；只恢复能力，不自动发起新对话。
     */
    void ResumeConversation();

    /**
     * 线程安全地请求切换对话状态；这里只置事件位，实际处理在 Run() 中完成。
     */
    void ToggleChatState();

    /**
     * 线程安全地请求开始监听；这里只置事件位，实际处理在 Run() 中完成。
     */
    void StartListening();

    /**
     * 线程安全地请求停止监听；这里只置事件位，实际处理在 Run() 中完成。
     */
    void StopListening();

    // 有序关闭网络音频和本地音频服务，延时后执行芯片复位。
    void Reboot();
    // 由非 AudioService 来源模拟一次唤醒词调用，仍复用正常建连与监听流程。
    void WakeWordInvoke(const std::string& wake_word);
    // 执行手动或自动固件升级，负责 UI、性能模式、音频停启和成功后的重启。
    bool UpgradeFirmware(const std::string& url, const std::string& version = "");
    // 状态机、协议音频通道和 AudioService 均空闲时才允许板级电源管理进入休眠。
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
    AudioService& GetAudioService() { return audio_service_; }
    
    /**
     * 线程安全地释放联网后创建的协议资源，包括关闭音频通道并销毁 Protocol 对象。
     * 可从任意任务调用，真正的释放动作会被投递到主任务。
     */
    void ResetProtocol();

private:
    Application();
    ~Application();

    // 仅保护跨任务投递队列；任务闭包执行前会整体移出并释放锁。
    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;

    // 联网激活后创建，所有权归 Application；销毁动作统一投递到主任务。
    std::unique_ptr<Protocol> protocol_;
    // 跨任务事件汇聚点以及每秒一次的状态栏/诊断时钟。
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
#ifdef CONFIG_ENABLE_VAD_BARGE_IN
    // VAD 首次检测到人声后延迟确认，过滤短促噪声和残余扬声器回声。
    esp_timer_handle_t barge_in_timer_handle_ = nullptr;
#endif

    // DeviceStateMachine 负责合法跳转校验，Application 负责跳转产生的实际副作用。
    DeviceStateMachine state_machine_;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    // WebRTC 独占音频链路期间为 true，用于从所有入口阻断新的助手对话。
    std::atomic<bool> conversation_suspended_{false};
    // 网络回调先保存错误文本再置 MAIN_EVENT_ERROR，由主任务统一展示。
    std::string last_error_message_;
    // 音频服务内部管理采集、播放、处理、唤醒与 Opus 编解码任务。
    AudioService audio_service_;
    // 仅在激活任务期间存在，激活完成后释放以回收 HTTP/配置相关内存。
    std::unique_ptr<Ota> ota_;

    std::function<void(const std::string&)> mcp_broadcast_callback_;

    bool has_server_time_ = false;  // 最近一次版本响应是否成功校准时间
    bool aborted_ = false;          // 本轮服务端 TTS 是否已被本地主动中止
    bool assets_version_checked_ = false;
    // 延迟到进入 Listening 且解码器重置完成后再播放提示音，避免提示音被清空。
    bool play_popup_on_listening_ = false;
    int clock_ticks_ = 0;
    // 防止网络事件重复创建多个并发激活任务。
    TaskHandle_t activation_task_handle_ = nullptr;


    // 主事件循环中的事件处理器。
    void HandleStateChangedEvent();
    void HandleToggleChatEvent();
    void HandleStartListeningEvent();
    void HandleStopListeningEvent();
    void HandleNetworkConnectedEvent();
    void HandleNetworkDisconnectedEvent();
    void HandleActivationDoneEvent();
    void HandleWakeWordDetectedEvent();
    void ContinueOpenAudioChannel(ListeningMode mode);
    void ContinueWakeWordInvoke(const std::string& wake_word);

    // 后台激活任务：资源更新 -> 版本/激活检查 -> 协议初始化，完成后通知主循环。
    void ActivationTask();

    // 启动阶段的三个顺序步骤，由低优先级 ActivationTask 调用。
    void CheckAssetsVersion();
    void CheckNewVersion();
    void InitializeProtocol();
    void ShowActivationCode(const std::string& code, const std::string& message);
    void SetListeningMode(ListeningMode mode);
    ListeningMode GetDefaultListeningMode() const;
    
    // 状态机监听器最终触发的 UI、音频策略更新入口。
    void OnStateChanged(DeviceState old_state, DeviceState new_state);
};


class TaskPriorityReset {
public:
    // RAII 临时调整当前任务优先级，离开作用域后自动恢复。
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;
};

#endif // _APPLICATION_H_
