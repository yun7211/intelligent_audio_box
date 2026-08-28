/* 门锁执行器。职责、线程约定与"不提供任意 GPIO 接口"的理由见 door_lock.h。 */
#include "door_lock.h"

#include <esp_log.h>

#define TAG "DoorLock"

DoorLock& DoorLock::GetInstance() {
    static DoorLock instance;
    return instance;
}

bool DoorLock::Init() {
    if (initialized_) {
        return true;
    }
    if (kGpio == GPIO_NUM_NC) {
        // 板子没接锁。不是错误，但门锁不可用 —— Open() 会据此回 error，而不是去操作 -1 号脚。
        ESP_LOGW(TAG, "No door lock GPIO configured for this board");
        return false;
    }

    // 顺序很重要：**先写锁存值，再配置成输出**。反过来的话，引脚从高阻切到输出的
    // 那一瞬间会先驱动出上一次残留的锁存值 —— 复位后它恰好可能是有效电平，等于
    // 开机自动开一次门。这一行就是为了消掉那个窗口。
    gpio_set_level(kGpio, kIdleLevel);

    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << (int)kGpio;
    cfg.mode         = GPIO_MODE_OUTPUT;
    cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
    // 内部下拉对推挽输出本身没有影响，留着是多一道纵深：万一引脚被别处改回输入，
    // 电平也是确定的低。真正覆盖复位期间高阻状态的，仍然必须是**外部**下拉电阻。
    cfg.pull_down_en = DOOR_LOCK_ACTIVE_LEVEL ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
    cfg.intr_type    = GPIO_INTR_DISABLE;
    if (gpio_config(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed for GPIO%d", (int)kGpio);
        return false;
    }
    gpio_set_level(kGpio, kIdleLevel);

    // 一次性定时器建一次就反复用 esp_timer_start_once 重启，不是每次开门都 create/delete：
    // 少一次可能失败的分配，也少一处忘记 delete 的泄漏点。
    esp_timer_create_args_t args = {};
    args.callback        = &DoorLock::TimerCb;
    args.arg             = this;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name            = "door_lock";
    if (esp_timer_create(&args, &timer_) != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create failed");
        timer_ = nullptr;
        return false;
    }

    initialized_ = true;
    ESP_LOGI(TAG, "Door lock ready on GPIO%d (active %d, pulse %ums)",
             (int)kGpio, kActiveLevel, (unsigned)kPulseMs);
    return true;
}

DoorLockResult DoorLock::Open() {
    // 允许迟到初始化：Enable() 时若 GPIO 初始化失败，这里每次开门请求会再试一次，
    // 仍然失败就老实回 error（设计文档 §9：初始化失败不影响通话，只让开门失败）。
    if (!initialized_ && !Init()) {
        return DoorLockResult::kError;
    }

    // 忙判定用 CAS，而不是"读-判断-写"：请求可能连着来两条，中间不能有窗口。
    // 命中忙的那一条**不重新计时**，所以重复点击不会把门开得更久。
    bool expected = false;
    if (!busy_.compare_exchange_strong(expected, true)) {
        return DoorLockResult::kBusy;
    }

    if (gpio_set_level(kGpio, kActiveLevel) != ESP_OK) {
        busy_.store(false);
        ESP_LOGE(TAG, "Failed to assert door lock GPIO");
        return DoorLockResult::kError;
    }
    if (esp_timer_start_once(timer_, (uint64_t)kPulseMs * 1000) != ESP_OK) {
        // 定时器起不来就等于**没有人会把电平拉回去**。宁可这次不开门，也不能留一个
        // 永远高着的引脚，所以立刻自己收回来再报错。
        gpio_set_level(kGpio, kIdleLevel);
        busy_.store(false);
        ESP_LOGE(TAG, "Failed to start pulse timer; GPIO restored");
        return DoorLockResult::kError;
    }
    return DoorLockResult::kOpened;
}

void DoorLock::ForceClose() {
    if (timer_ != nullptr) {
        // 定时器没在跑时返回 ESP_ERR_INVALID_STATE，这里无需理会 —— 目标只是"别再有
        // 回调把电平改回去"，已经停了就是达成状态。
        esp_timer_stop(timer_);
    }
    if (initialized_) {
        gpio_set_level(kGpio, kIdleLevel);
    }
    busy_.store(false);
}

void DoorLock::TimerCb(void* arg) {
    static_cast<DoorLock*>(arg)->Expire();
}

void DoorLock::Expire() {
    // esp_timer 任务上下文：只做两件最短的事。与 ForceClose() 撞车时两边写的是同一个
    // 无效电平，结果一致，因此不需要额外加锁。
    gpio_set_level(kGpio, kIdleLevel);
    busy_.store(false);
}
