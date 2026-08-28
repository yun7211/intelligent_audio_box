#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"

#define TAG "main"

/*
 * ESP-IDF 固件入口。
 *
 * 启动链路：Bootloader 选择应用分区 -> ESP-IDF 完成系统组件初始化 -> 调用 app_main() ->
 * 初始化 NVS -> 获取 Application 单例 -> 装配硬件/音频/网络 -> 进入主事件循环。
 *
 * app_main() 本身不承载业务逻辑。所有长期运行的状态管理都交给 Application，这样入口只负责
 * 建立业务运行所需的最小基础环境。若此函数执行返回，意味着主程序异常退出。
 */
extern "C" void app_main(void)
{
    /*
     * 第 1 步：初始化 NVS。
     * NVS 保存 Wi-Fi 凭据、音量、主题、OTA 地址等跨重启配置。以下两种返回值表示现有分区
     * 无法继续使用：页面已耗尽，或分区格式来自不兼容的新版本。此时擦除并重新初始化，
     * 代价是恢复默认配置，但可以避免设备永久卡在启动阶段。
     */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /*
     * 第 2 步：初始化整机业务。
     * Application 使用函数内静态对象实现单例，首次调用时构造事件组和时钟定时器。
     * Initialize() 装配显示、音频、MCP、网络回调并异步启动联网；它不会等待网络成功。
     */
    auto& app = Application::GetInstance();
    app.Initialize();

    /*
     * 第 3 步：当前 app_main 任务转为业务主任务。
     * Run() 永久等待 FreeRTOS 事件位，并串行处理跨任务请求；正常情况下不会返回。
     */
    app.Run();
}
