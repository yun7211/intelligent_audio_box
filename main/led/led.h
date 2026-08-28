#ifndef _LED_H_
#define _LED_H_

class Led {
public:
    virtual ~Led() = default;
    // 根据 Application 当前设备状态更新灯效。
    virtual void OnStateChanged() = 0;
};


class NoLed : public Led {
    // 空对象实现，让无 LED 板型无需在业务层反复判空。
public:
    virtual void OnStateChanged() override {}
};

#endif // _LED_H_
