#pragma once

#include <functional>

#include <esp_timer.h>
#include <esp_pm.h>

// 旧版两级睡眠计时器：空闲达到阈值后先进入 light sleep，继续超时可请求 deep sleep。
// 回调只表达策略节点，具体关闭屏幕、网络或外设由板级代码处理。
class SleepTimer {
public:
    SleepTimer(int seconds_to_light_sleep = 20, int seconds_to_deep_sleep = -1);
    ~SleepTimer();

    void SetEnabled(bool enabled);
    void OnEnterLightSleepMode(std::function<void()> callback);
    void OnExitLightSleepMode(std::function<void()> callback);
    void OnEnterDeepSleepMode(std::function<void()> callback);
    void WakeUp();

private:
    void CheckTimer();

    esp_timer_handle_t sleep_timer_ = nullptr;
    bool enabled_ = false;
    int ticks_ = 0;
    int seconds_to_light_sleep_;
    int seconds_to_deep_sleep_;
    bool in_light_sleep_mode_ = false;

    std::function<void()> on_enter_light_sleep_mode_;
    std::function<void()> on_exit_light_sleep_mode_;
    std::function<void()> on_enter_deep_sleep_mode_;
};
