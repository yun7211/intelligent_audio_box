#ifndef KNOB_H_
#define KNOB_H_

#include <driver/gpio.h>
#include <functional>
#include <esp_log.h>
#include <iot_knob.h>

// 旋转编码器的轻量封装：把 iot_knob 左/右事件转换成一个方向布尔值回调。
class Knob {
public:
    Knob(gpio_num_t pin_a, gpio_num_t pin_b);
    ~Knob();

    void OnRotate(std::function<void(bool)> callback);

private:
    static void knob_callback(void* arg, void* data);

    knob_handle_t knob_handle_;
    gpio_num_t pin_a_;
    gpio_num_t pin_b_;
    std::function<void(bool)> on_rotate_;
};

#endif // KNOB_H_
