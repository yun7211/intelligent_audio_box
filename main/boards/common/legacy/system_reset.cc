#include "system_reset.h"

#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>


#define TAG "SystemReset"

// 上电早期读取两个低有效按键：一个只清 NVS，另一个同时清 OTA 选择信息以回到出厂固件。
SystemReset::SystemReset(gpio_num_t reset_nvs_pin, gpio_num_t reset_factory_pin) : reset_nvs_pin_(reset_nvs_pin), reset_factory_pin_(reset_factory_pin) {
    // 配置为上拉输入，未按下时保持高电平。
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << reset_nvs_pin_) | (1ULL << reset_factory_pin_);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);
}


void SystemReset::CheckButtons() {
    if (gpio_get_level(reset_factory_pin_) == 0) {
        ESP_LOGI(TAG, "Button is pressed, reset to factory");
        ResetNvsFlash();
        ResetToFactory();
    }

    if (gpio_get_level(reset_nvs_pin_) == 0) {
        ESP_LOGI(TAG, "Button is pressed, reset NVS flash");
        ResetNvsFlash();
    }
}

void SystemReset::ResetNvsFlash() {
    ESP_LOGI(TAG, "Resetting NVS flash");
    esp_err_t ret = nvs_flash_erase();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase NVS flash");
    }
    ret = nvs_flash_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS flash");
    }
}

void SystemReset::ResetToFactory() {
    ESP_LOGI(TAG, "Resetting to factory");
    // 擦除 otadata 后，Bootloader 会按默认规则重新选择应用分区。
    const esp_partition_t* partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (partition == NULL) {
        ESP_LOGE(TAG, "Failed to find otadata partition");
        return;
    }
    esp_partition_erase_range(partition, 0, partition->size);
    ESP_LOGI(TAG, "Erased otadata partition");

    // 留出日志和界面提示时间后重启。
    RestartInSeconds(3);
}

void SystemReset::RestartInSeconds(int seconds) {
    for (int i = seconds; i > 0; i--) {
        ESP_LOGI(TAG, "Resetting in %d seconds", i);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    esp_restart();
}
