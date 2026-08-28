#ifndef WIFI_BOARD_H
#define WIFI_BOARD_H

#include "board.h"
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>

/*
 * WifiBoard 在 Board 抽象上补充 WiFi 生命周期：连接、60 秒超时、进入配网以及事件转发。
 * 具体板型只需要提供显示、音频和按键等硬件，网络流程由该基类复用。
 */
class WifiBoard : public Board {
protected:
    esp_timer_handle_t connect_timer_ = nullptr;
    bool in_config_mode_ = false;
    NetworkEventCallback network_event_callback_ = nullptr;

    virtual std::string GetBoardJson() override;

    /**
     * 接收 WifiManager 回调并转成统一 NetworkEvent。
     * @param data SSID 等附加信息。
     */
    void OnNetworkEvent(NetworkEvent event, const std::string& data = "");

    /**
     * 发起一次 WiFi 连接尝试并启动超时计时。
     */
    void TryWifiConnect();

    /**
     * 进入当前配置选择的 WiFi 配网方式。
     */
    void StartWifiConfigMode();

    /**
     * 连接超时回调：放弃当前尝试并转入配网模式。
     */
    static void OnWifiConnectTimeout(void* arg);

public:
    WifiBoard();
    virtual ~WifiBoard();
    
    virtual std::string GetBoardType() override;
    
    /**
     * Start network connection asynchronously
     * This function returns immediately. Network events are notified through the callback set by SetNetworkEventCallback().
     */
    virtual void StartNetwork() override;
    
    virtual NetworkInterface* GetNetwork() override;
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) override;
    virtual const char* GetNetworkStateIcon() override;
    virtual void SetPowerSaveLevel(PowerSaveLevel level) override;
    virtual AudioCodec* GetAudioCodec() override { return nullptr; }
    virtual std::string GetDeviceStatusJson() override;
    
    /**
     * Enter WiFi configuration mode (thread-safe, can be called from any task)
     */
    void EnterWifiConfigMode();
    
    /**
     * Check if in WiFi config mode
     */
    bool IsInWifiConfigMode() const;
};

#endif // WIFI_BOARD_H
