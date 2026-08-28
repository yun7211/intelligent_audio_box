#pragma once

#include <aes/esp_aes.h>
#include <cassert>
#include <cstring>
#include <vector>
#include "esp_blufi_api.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/aes.h"
#include "mbedtls/dhm.h"
#include "wifi_manager.h"

// ESP-BLUFI 蓝牙配网管理器：负责 BLE 协议栈生命周期、安全协商、Wi-Fi 扫描与凭据下发。
// ESP-IDF 使用 C 回调，因此 trampoline 静态函数会把事件转交给单例实例处理。
class Blufi {
public:
    /**
     * @brief 获取 BLUFI 全局单例。
     */
    static Blufi &GetInstance();

    /**
     * @brief 为手机配网页启动 Wi-Fi 扫描。
     * 配网模式下优先复用已有结果；其他状态单独扫描，避免干扰正常连接。
     */
    bool start_wifi_scan();

    /**
     * @brief 依次初始化蓝牙控制器、Host 和 BLUFI Profile，是配网流程入口。
     */
    esp_err_t init();

    /**
     * @brief 释放 BLUFI Profile 与蓝牙协议栈资源。
     */
    esp_err_t deinit();

    // 单例持有协议栈全局状态，禁止复制。
    Blufi(const Blufi &) = delete;

    Blufi &operator=(const Blufi &) = delete;

private:
    bool inited_ = false;

    Blufi();

    ~Blufi();

    // 控制器、Host 与 GAP 的分层初始化/反初始化步骤。
    static esp_err_t _controller_init();

    static esp_err_t _controller_deinit();

    static esp_err_t _host_init();

    static esp_err_t _host_deinit();

    static esp_err_t _gap_register_callback();

    static esp_err_t _host_and_cb_init();

    void _security_init();

    void _security_deinit();

    void _dh_negotiate_data_handler(uint8_t *data, int len, uint8_t **output_data, int *output_len,
                                    bool *need_free);

    int _aes_encrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len);

    int _aes_decrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len);

    static uint16_t _crc_checksum(uint8_t iv8, uint8_t *data, int len);

    void _handle_event(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param);

    static int _get_softap_conn_num();

    // Wi-Fi 扫描结果最终通过 BLUFI 事件回传给手机。
    void _send_wifi_list();
    void _start_dedicated_wifi_scan();
    static void _wifi_scan_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                                         void *event_data);

    // 注册给 ESP-IDF 的 C 风格跳板回调，内部转发到当前实例。

    static void _event_callback_trampoline(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param);

    static void _negotiate_data_handler_trampoline(uint8_t *data, int len, uint8_t **output_data,
                                                   int *output_len, bool *need_free);

    static int _encrypt_func_trampoline(uint8_t iv8, uint8_t *crypt_data, int crypt_len);

    static int _decrypt_func_trampoline(uint8_t iv8, uint8_t *crypt_data, int crypt_len);

    static uint16_t _checksum_func_trampoline(uint8_t iv8, uint8_t *data, int len);

#ifdef CONFIG_BT_NIMBLE_ENABLED
    static void _nimble_on_reset(int reason);
    static void _nimble_on_sync();
    static void _nimble_host_task(void *param);
#endif

    // BLUFI 安全上下文：DH 协商共享密钥，再派生 AES 会话密钥保护配网数据。
    struct BlufiSecurity {
#define DH_SELF_PUB_KEY_LEN 128
        uint8_t self_public_key[DH_SELF_PUB_KEY_LEN];
#define SHARE_KEY_LEN 128
        uint8_t share_key[SHARE_KEY_LEN];
        size_t share_len;
#define PSK_LEN 16
        uint8_t psk[PSK_LEN];
        uint8_t *dh_param;
        int dh_param_len;
        uint8_t iv[16];
        mbedtls_dhm_context *dhm;
        esp_aes_context *aes;
    };

    BlufiSecurity *m_sec;

    // 手机连接、STA 联网和凭据下发过程的状态快照。
    wifi_config_t m_sta_config{};
    bool m_ble_is_connected;
    bool m_sta_connected;
    bool m_sta_got_ip;
    bool m_provisioned;
    bool m_deinited;
    uint8_t m_sta_bssid[6]{};
    uint8_t m_sta_ssid[32]{};
    int m_sta_ssid_len;
    bool m_sta_is_connecting;
    esp_blufi_extra_info_t m_sta_conn_info{};

    // 扫描缓存及延迟响应标记，避免连接过程中的扫描覆盖手机请求的结果。
    std::vector<wifi_ap_record_t> m_ap_records;
    bool m_scan_in_progress = false;
    // 为 true 时保存下一次扫描结果；连接 AP 前清零，避免连接扫描污染展示缓存。
    bool m_scan_should_save_ssid = true;
    // 手机请求列表但结果尚未就绪时置位；扫描完成并回包后清零。
    bool m_send_list_after_scan = false;
};
