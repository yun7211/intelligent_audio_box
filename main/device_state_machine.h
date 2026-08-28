#ifndef DEVICE_STATE_MACHINE_H
#define DEVICE_STATE_MACHINE_H

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

#include "device_state.h"

/**
 * 设备状态机：集中校验状态跳转，并通过观察者回调通知 UI、音频等模块。
 * 状态值使用原子变量供跨任务读取；监听器列表由互斥锁保护。
 */
class DeviceStateMachine {
public:
    DeviceStateMachine();
    ~DeviceStateMachine() = default;

    // 状态机持有监听器和互斥锁，不允许复制。
    DeviceStateMachine(const DeviceStateMachine&) = delete;
    DeviceStateMachine& operator=(const DeviceStateMachine&) = delete;

    /**
     * 原子读取当前设备状态。
     */
    DeviceState GetState() const { return current_state_.load(); }

    /**
     * 尝试切换到目标状态。
     * @return 跳转合法（或目标与当前状态相同）时返回 true，否则返回 false。
     */
    bool TransitionTo(DeviceState new_state);

    /**
     * 仅检查从当前状态跳到目标状态是否合法，不产生状态变化。
     */
    bool CanTransitionTo(DeviceState target) const;

    /**
     * 状态变化回调，参数依次为旧状态、新状态。
     */
    using StateCallback = std::function<void(DeviceState, DeviceState)>;

    /**
     * 添加状态变化监听器。回调在调用 TransitionTo() 的任务上下文中同步执行，
     * 若监听器需要操作 UI 或主业务状态，应自行投递到主任务。
     * @return 可用于移除监听器的编号。
     */
    int AddStateChangeListener(StateCallback callback);

    /**
     * 按编号移除状态变化监听器。
     */
    void RemoveStateChangeListener(int listener_id);

    /**
     * 获取用于日志输出的状态名称。
     */
    static const char* GetStateName(DeviceState state);

private:
    std::atomic<DeviceState> current_state_{kDeviceStateUnknown};
    std::vector<std::pair<int, StateCallback>> listeners_;
    int next_listener_id_{0};
    std::mutex mutex_;

    /**
     * 校验指定的源状态到目标状态是否合法。
     */
    bool IsValidTransition(DeviceState from, DeviceState to) const;

    /**
     * 通知状态变化监听器；实现会先复制回调列表，避免持锁执行外部代码。
     */
    void NotifyStateChange(DeviceState old_state, DeviceState new_state);
};

#endif // DEVICE_STATE_MACHINE_H
