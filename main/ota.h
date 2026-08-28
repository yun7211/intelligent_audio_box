#ifndef _OTA_H
#define _OTA_H

#include <functional>
#include <string>

#include <esp_err.h>
#include "board.h"

/**
 * OTA 与设备激活客户端。
 *
 * 该类包含两个相互分离的阶段：
 * 1. 控制阶段：CheckVersion() 向版本服务上报设备信息，解析固件、激活、服务器时间以及
 *    MQTT/WebSocket 配置。这个阶段只更新内存状态和 NVS，不写应用分区。
 * 2. 数据阶段：Upgrade()/StartUpgrade() 通过 HTTP 流式下载固件，将镜像写入非运行 OTA
 *    分区，校验成功后把它设为下次启动分区，但不会在类内部直接重启。
 *
 * Application 负责决定调用顺序、重试策略、UI 提示和重启时机；Ota 只负责单次网络交换
 * 与底层 OTA 写入。
 */
class Ota {
public:
    Ota();
    ~Ota();

    // 请求版本服务并解析本轮启动需要的全部配置。
    esp_err_t CheckVersion();
    // 使用 CheckVersion() 返回的 challenge 完成一次设备激活请求。
    esp_err_t Activate();

    // 以下 Has* 接口描述最近一次 CheckVersion() 响应中包含了哪些能力或动作。
    bool HasActivationChallenge() { return has_activation_challenge_; }
    bool HasNewVersion() { return has_new_version_; }
    bool HasMqttConfig() { return has_mqtt_config_; }
    bool HasWebsocketConfig() { return has_websocket_config_; }
    bool HasActivationCode() { return has_activation_code_; }
    bool HasServerTime() { return has_server_time_; }
    // 使用服务端返回的 firmware_url_ 升级。
    bool StartUpgrade(std::function<void(int progress, size_t speed)> callback);
    // 从指定 URL 升级；静态接口也供 MCP/手动升级流程直接调用。
    static bool Upgrade(const std::string& firmware_url, std::function<void(int progress, size_t speed)> callback);
    // 设备已稳定运行后取消 Bootloader 的待验证/自动回滚状态。
    void MarkCurrentVersionValid();

    const std::string& GetFirmwareVersion() const { return firmware_version_; }
    const std::string& GetCurrentVersion() const { return current_version_; }
    const std::string& GetFirmwareUrl() const { return firmware_url_; }
    const std::string& GetActivationMessage() const { return activation_message_; }
    const std::string& GetActivationCode() const { return activation_code_; }
    // 版本服务地址优先取 NVS 配置，未配置时使用固件 Kconfig 默认值。
    std::string GetCheckVersionUrl();

private:
    // 激活信息：传统设备使用 code，带出厂序列号的设备使用 challenge/HMAC 证明身份。
    std::string activation_message_;
    std::string activation_code_;
    // 最近一次响应的字段存在标记。每次 CheckVersion() 都会重新计算相应标记。
    bool has_new_version_ = false;
    bool has_mqtt_config_ = false;
    bool has_websocket_config_ = false;
    bool has_server_time_ = false;
    bool has_activation_code_ = false;
    bool has_serial_number_ = false;
    bool has_activation_challenge_ = false;
    // 当前运行镜像版本以及服务端候选镜像的版本和下载地址。
    std::string current_version_;
    std::string firmware_version_;
    std::string firmware_url_;
    std::string activation_challenge_;
    std::string serial_number_;
    // 服务端建议的激活等待时间；当前重试节奏由 Application 统一控制。
    int activation_timeout_ms_ = 30000;

    std::function<void(int progress, size_t speed)> upgrade_callback_;
    // 把点分版本号拆成整数段，并按段判断候选版本是否更新。
    std::vector<int> ParseVersion(const std::string& version);
    bool IsNewVersionAvailable(const std::string& currentVersion, const std::string& newVersion);
    // 生成带序列号、challenge 和 HMAC 结果的激活 JSON。
    std::string GetActivationPayload();
    // 创建带统一设备身份、语言和内容类型请求头的 HTTP 客户端。
    std::unique_ptr<Http> SetupHttp();
};

#endif // _OTA_H
