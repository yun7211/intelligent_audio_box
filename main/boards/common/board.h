#ifndef BOARD_H
#define BOARD_H

#include <http.h>
#include <web_socket.h>
#include <mqtt.h>
#include <udp.h>
#include <string>
#include <functional>
#include <network_interface.h>

#include "led/led.h"
#include "backlight.h"
#include "camera.h"
#include "assets.h"

/** 统一网络事件，屏蔽 WiFi 等具体网络实现差异。 */
enum class NetworkEvent {
    Scanning,              // 正在扫描网络
    Connecting,            // 正在连接，data 为 SSID/网络名
    Connected,             // 连接成功，data 为 SSID/网络名
    Disconnected,          // 网络断开
    WifiConfigModeEnter,   // 已进入 WiFi 配网模式
    WifiConfigModeExit     // 已退出 WiFi 配网模式
};

// 整机功耗策略，由 Application 在待机、联网和升级等阶段切换。
enum class PowerSaveLevel {
    LOW_POWER,    // 最大节能
    BALANCED,     // 功耗与性能平衡
    PERFORMANCE,  // 全性能运行
};

// 网络事件统一回调；data 携带 SSID 等事件附加信息。
using NetworkEventCallback = std::function<void(NetworkEvent event, const std::string& data)>;

void* create_board();
class AudioCodec;
class Display;
// Board 是硬件能力总入口；每个板型只实例化一个派生类，并向业务层提供统一外设接口。
class Board {
private:
    Board(const Board&) = delete; // 禁用拷贝构造函数
    Board& operator=(const Board&) = delete; // 禁用赋值操作

protected:
    Board();
    std::string GenerateUuid();

    // 软件生成的设备唯一标识
    std::string uuid_;

public:
    static Board& GetInstance() {
        static Board* instance = static_cast<Board*>(create_board());
        return *instance;
    }

    virtual ~Board() = default;
    virtual std::string GetBoardType() = 0;
    virtual std::string GetUuid() { return uuid_; }
    virtual Backlight* GetBacklight() { return nullptr; }
    virtual Led* GetLed();
    virtual AudioCodec* GetAudioCodec() = 0;
    virtual bool GetTemperature(float& esp32temp);
    virtual Display* GetDisplay();
    virtual Camera* GetCamera();
    virtual NetworkInterface* GetNetwork() = 0;
    virtual void StartNetwork() = 0;
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) { (void)callback; }
    virtual const char* GetNetworkStateIcon() = 0;
    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging);
    virtual std::string GetSystemInfoJson();
    virtual void SetPowerSaveLevel(PowerSaveLevel level) = 0;
    virtual std::string GetBoardJson() = 0;
    virtual std::string GetDeviceStatusJson() = 0;
};

#define DECLARE_BOARD(BOARD_CLASS_NAME) \
void* create_board() { \
    return new BOARD_CLASS_NAME(); \
}

#endif // BOARD_H
