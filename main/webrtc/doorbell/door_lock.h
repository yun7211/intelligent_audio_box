/* 门锁执行器 —— 把一次"开门"翻译成 GPIO 上的一次定长高电平脉冲。
 *
 * 为什么值得单独一个类：开门是本项目唯一会**驱动外部电路**的动作，它的失效模式
 * （电平卡在有效位上 = 门一直开着）比任何一个纯软件 bug 都严重。把它从门铃业务
 * 逻辑里剥出来，"任何路径下最终都回到无效电平"这条不变量就只需要在这一个文件里
 * 成立，而不用在每个调用点上重新论证一遍。
 *
 * 这里刻意**不提供任意 GPIO 读写**：编号、有效电平和脉冲宽度都是编译期常量，
 * 外部只能请求"开一次门"。对端就算完全被攻破，也拿不到一个通用的引脚操作接口。
 *
 * 线程约定：
 *  - Init() / Open() / ForceClose() 由主任务调用（DoorbellController 已经做过跳线程）。
 *  - Expire() 在 esp_timer 的回调任务里跑。**回调里绝对不能 vTaskDelay()**：
 *    esp_timer 的回调都在同一个高优先级任务上串行执行，阻塞它会连带拖住整机所有
 *    定时器（包括 WebRTC 和音频自己的），所以这里只做 gpio_set_level + 清标志。
 *  - busy_ 因此必须是 atomic：主任务置位，定时器任务清零。
 */
#ifndef WEBRTC_DOOR_LOCK_H
#define WEBRTC_DOOR_LOCK_H

#include <atomic>

#include <driver/gpio.h>
#include <esp_timer.h>

// 引脚参数来自板级 config.h（见 boards/lichuang-dev/config.h）。门铃模式并不限定板型，
// 所以缺宏时要能编过 —— 但兜底值是 GPIO_NUM_NC，而**不是**随便挑一个引脚：没在板级配置
// 里声明过锁的板子，宁可让远程开门直接不可用（Init() 报 warning、Open() 回 error），
// 也绝不去驱动一个板主没同意交出来的引脚。
#include "config.h"

#ifndef DOOR_LOCK_GPIO
#define DOOR_LOCK_GPIO GPIO_NUM_NC
#endif
#ifndef DOOR_LOCK_ACTIVE_LEVEL
#define DOOR_LOCK_ACTIVE_LEVEL 1
#endif
#ifndef DOOR_LOCK_PULSE_MS
#define DOOR_LOCK_PULSE_MS 800
#endif

// Open() 的三种结局，与 DataChannel 上回给浏览器的 status 一一对应。
//   kOpened —— 已开始脉冲；kBusy —— 上一次脉冲未结束（**不重新计时**）；
//   kError  —— GPIO 或定时器失败，电平已被强制拉回无效态。
enum class DoorLockResult { kOpened, kBusy, kError };

class DoorLock {
public:
    static DoorLock& GetInstance();

    bool Init();                 // 幂等；失败时门锁不可用，但通话功能不受影响
    DoorLockResult Open();       // 请求一次脉冲
    void ForceClose();           // 无条件回到无效电平（停止服务 / 退出门铃 / 出错时调用）
    bool busy() const { return busy_.load(); }

private:
    DoorLock() = default;
    static void TimerCb(void* arg);
    void Expire();               // 在 esp_timer 任务上跑

    static constexpr gpio_num_t kGpio        = DOOR_LOCK_GPIO;
    static constexpr int        kActiveLevel = DOOR_LOCK_ACTIVE_LEVEL;
    static constexpr int        kIdleLevel   = DOOR_LOCK_ACTIVE_LEVEL ? 0 : 1;
    static constexpr uint32_t   kPulseMs     = DOOR_LOCK_PULSE_MS;

    bool initialized_ = false;             // 只在主任务上读写
    esp_timer_handle_t timer_ = nullptr;   // 一次性定时器，建一次反复复用
    std::atomic<bool> busy_{false};
};

#endif // WEBRTC_DOOR_LOCK_H
