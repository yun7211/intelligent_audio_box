#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#if CONFIG_USE_WEBRTC_CALL
#include "webrtc/webrtc_mcp_tools.h"
#endif
#include "assets.h"
#include "settings.h"

#include <cstring>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>

#define TAG "Application"

/*
 * Application 是整机业务编排层：硬件回调、网络回调和音频任务只产生事件，Run() 在主任务
 * 中串行消费这些事件，再驱动状态机、显示、音频和协议层。这样既保留各模块的异步能力，
 * 又避免它们从不同任务直接争用业务状态或 UI。
 */

Application::Application() {
    /*
     * 构造函数只创建不依赖具体板型的调度基础设施。Board 外设在 Initialize() 中获取，避免
     * 静态对象初始化顺序导致驱动尚未就绪。event_group_ 是所有异步模块唤醒主任务的入口。
     */
    event_group_ = xEventGroupCreate();

    // AEC 位置是互斥的编译期能力，同时启用设备端和服务端会让音频格式/监听策略产生歧义。
#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    /*
     * esp_timer 回调运行在系统定时器任务中，不能直接更新 LVGL。这里只置 CLOCK_TICK 位，
     * Run() 再执行状态栏刷新和周期性诊断。
     */
    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);

#ifdef CONFIG_ENABLE_VAD_BARGE_IN
    esp_timer_create_args_t barge_in_timer_args = {
        .callback = [](void* arg) {
            auto app = static_cast<Application*>(arg);
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_BARGE_IN_CONFIRMED);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "barge_in_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&barge_in_timer_args, &barge_in_timer_handle_);
#endif
}

Application::~Application() {
#ifdef CONFIG_ENABLE_VAD_BARGE_IN
    if (barge_in_timer_handle_ != nullptr) {
        esp_timer_stop(barge_in_timer_handle_);
        esp_timer_delete(barge_in_timer_handle_);
    }
#endif
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) {
    return state_machine_.TransitionTo(state);
}

void Application::Initialize() {
    /*
     * 初始化顺序刻意保持为：状态机 -> 显示 -> 音频 -> 回调 -> 定时器/MCP -> 网络。
     * 显示先于可能失败的模块创建，保证后续联网或激活错误可见；音频先于联网启动，保证
     * 配网提示音和按键音可用；网络最后异步启动，避免回调访问尚未装配的对象。
     */
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // 先建立 UI，后续初始化阶段的状态和错误才能立即显示出来。
    auto display = board.GetDisplay();
    display->SetupUI();
    // 启动页显示板型与固件版本，便于现场确认设备身份。
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // AudioService 接管 codec，并启动采集、播放和 Opus 编解码任务。
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    /*
     * AudioService 的采集、唤醒、VAD 回调来自音频任务。回调不发送网络数据、不切状态，
     * 只把事实转换为事件位：发送队列由 Run() 排空，唤醒词和 VAD 也由主任务解释。
     */
    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    // 状态机回调可能来自任意调用 TransitionTo() 的任务，因此这里只置事件位，
    // 具体的 UI/音频切换统一留给主循环。
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // 每秒产生一次时钟事件，用于刷新状态栏和周期性运行轻量维护工作。
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // MCP 工具在协议建连前注册，服务端首次读取工具列表时即可拿到完整能力集合。
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();
#if CONFIG_USE_WEBRTC_CALL
    // 注册独立的 WebRTC 通话工具（self.video_call.*）。
    RegisterWebrtcMcpTools();
#endif

    /*
     * 网络回调可能来自 Wi-Fi 驱动任务。Connecting 只更新短期提示；Connected/Disconnected
     * 置主事件位，让 Run() 决定是否启动激活任务或关闭对话通道。配网模式自身由 Board 管理。
     */
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();
        
        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // 配网模式的进入流程由 WifiBoard 内部处理。
                break;
            case NetworkEvent::WifiConfigModeExit:
                // 配网模式的退出流程由 WifiBoard 内部处理。
                break;
        }
    });

    // 异步启动网络；连接结果稍后通过上面的回调返回。
    board.StartNetwork();

    // 不等首个定时器 tick，立即把当前网络状态显示出来。
    display->UpdateStatusBar(true);
}

void Application::Run() {
    /*
     * app_main 从这里开始成为永久业务主循环。一次 WaitBits 可以返回多个事件，处理顺序为：
     * 错误/网络/激活 -> 状态和用户控制 -> 音频发送/唤醒/VAD -> 投递闭包 -> 周期维护。
     * 前置事件可能改变状态，后置处理会看到最新状态，从而避免在断网后继续发送音频等情况。
     */
    // 主任务负责状态机和跨模块编排，提高优先级以避免被后台编解码/激活任务长期饿死。
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS = 
        MAIN_EVENT_SCHEDULE |
        MAIN_EVENT_SEND_AUDIO |
        MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE |
        MAIN_EVENT_CLOCK_TICK |
        MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED |
        MAIN_EVENT_NETWORK_DISCONNECTED |
        MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING |
        MAIN_EVENT_STOP_LISTENING |
        MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED |
        MAIN_EVENT_BARGE_IN_CONFIRMED;

    while (true) {
        // pdTRUE 会在返回时清除已消费的事件位；一次唤醒可批量处理同时发生的多个事件。
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            // 网络协议错误统一结束当前会话并进入可恢复的 Idle，再显示错误而非进入致命状态。
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            // 一次事件尽量排空发送队列；协议发送失败则停止，等待重连或下一次事件。
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            // VAD 状态保存在 AudioService，事件本身不携带数据。
            auto state = GetDeviceState();
            if (state == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }

#ifdef CONFIG_ENABLE_VAD_BARGE_IN
            if (state == kDeviceStateSpeaking && barge_in_timer_handle_ != nullptr) {
                // 人声开始时重新启动单次计时；提前恢复静音则取消本次候选打断。
                esp_timer_stop(barge_in_timer_handle_);
                if (audio_service_.IsVoiceDetected()) {
                    esp_timer_start_once(
                        barge_in_timer_handle_,
                        CONFIG_VAD_BARGE_IN_CONFIRM_MS * 1000);
                }
            }
#endif
        }

        if (bits & MAIN_EVENT_BARGE_IN_CONFIRMED) {
#ifdef CONFIG_ENABLE_VAD_BARGE_IN
            // 定时器到期后再次核对状态，避免 TTS 已正常结束或短促噪声造成过期打断。
            if (GetDeviceState() == kDeviceStateSpeaking &&
                audio_service_.IsVoiceDetected() &&
                !conversation_suspended_.load()) {
                ESP_LOGI(TAG, "User speech confirmed, interrupting TTS");
                AbortSpeaking(kAbortReasonNone);
                audio_service_.EnableBargeInDetection(false);
                // 服务端 abort 只能阻止后续 TTS，本地已缓存的解码/播放数据必须立即丢弃。
                audio_service_.ResetDecoder();

                // 丢弃上一阶段残留的上行音频，下一轮 Listening 从用户当前语句继续采集。
                while (audio_service_.PopPacketFromSendQueue());
                SetListeningMode(GetDefaultListeningMode());
            }
#endif
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            // 整批移出队列后立即释放锁，回调里再次 Schedule() 不会死锁，且新任务留到下轮处理。
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
        
            // 每 10 秒输出一次堆内存统计，用于观察长期运行中的泄漏或碎片化。
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
            }
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    /*
     * 只有启动或配网状态下的首次联网才进入激活链路。正常运行中的网络重连只刷新状态栏，
     * 已创建的 Protocol 由自身重连机制恢复，避免重复下载资源或并发创建协议对象。
     */
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // 网络就绪后把耗时的激活/升级检查放到低优先级后台任务，主循环继续响应 UI。
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        // 激活含 HTTP 重试和 Flash 下载，放到低优先级独立任务，不能阻塞主事件循环。
        xTaskCreate([](void* arg) {
            Application* app = static_cast<Application*>(arg);
            app->ActivationTask();
            app->activation_task_handle_ = nullptr;
            vTaskDelete(NULL);
        }, "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    // 网络变化需要立即刷新，不等待下一个时钟事件。
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    // 对话依赖网络通道；断网时主动关闭，避免状态机一直停在连接/监听/播报态。
    auto state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening || state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        protocol_->CloseAudioChannel();
    }

    // 立即刷新断网图标。
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    /*
     * ActivationTask 只做耗时工作并置位，所有收尾在主任务完成：进入 Idle、保存时间可信标志、
     * 展示当前版本、释放 Ota 对象、恢复低功耗，并播放启动成功提示。
     */
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();

    auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");

    // 激活完成后释放 OTA 对象，归还其 HTTP 缓冲等内存。
    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    Schedule([this]() {
        // 回到主任务播放就绪提示音，避免后台激活任务直接操作音频队列。
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    });
}

void Application::ActivationTask() {
    /*
     * 联网后的后台启动流水线。三个步骤按顺序执行是因为：
     * - 资源包可能包含新版固件需要的界面、字体和提示音；
     * - 版本响应同时下发激活状态和协议配置；
     * - Protocol 必须在配置写入 NVS 后创建。
     * 任一步的可恢复错误都不会让任务悬空，最后仍通知主循环完成启动。
     */
    // OTA 对象同时承载版本检查、激活信息和服务器下发的协议配置。
    ota_ = std::make_unique<Ota>();

    // 先更新资源，再检查固件，确保新版界面/音效可随激活流程应用。
    CheckAssetsVersion();

    // 固件升级成功会重启；失败则继续后续初始化，设备仍可使用。
    CheckNewVersion();

    // 根据 OTA 配置选择 MQTT+UDP 或 WebSocket 通信实现。
    InitializeProtocol();

    // 后台任务不直接切状态，通过事件通知主循环收尾。
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    /*
     * 资源更新与固件 OTA 相互独立。下载地址由服务器提前写入 assets/download_url；读取后
     * 立即删除使其具备“一次性命令”语义。下载失败保留旧资源分区并回到 Activating，
     * 不影响后续固件检查和协议初始化。
     */
    // 单次启动只检查一次资源，避免激活流程重试时重复下载或应用。
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // download_url 是一次性指令，读取后立即删除，防止重启后重复下载。
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // 留出时间播完升级提示音，再进入资源下载阶段。
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        // 下载回调运行在激活任务中，显示更新通过 Schedule() 回到主任务。
        bool success = assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            Schedule([display, message = std::string(buffer)]() {
                display->SetChatMessage("system", message.c_str());
            });
        });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // 无论是否下载，都应用当前有效资源分区。
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion() {
    /*
     * 版本/激活循环同时解决四件事：
     * 1. 获取服务器配置；2. 必要时升级固件；3. 确认当前 OTA 镜像可用；4. 等待设备激活。
     *
     * CheckVersion() 的网络失败最多重试 10 次，并采用 10、20、40... 秒指数退避。用户在
     * 等待期间通过交互把状态切到 Idle 时，只中断当次等待，不破坏后台任务收尾。
     */
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // 初始重试间隔（秒）

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // 指数退避，避免故障期间频繁请求服务器
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // 成功后恢复初始重试间隔

        // 升级成功路径会在 UpgradeFirmware() 内重启；返回 false 表示继续使用当前固件启动。
        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return; // 升级成功会重启，通常不会执行到这里
            }
            // 升级失败时继续正常启动，避免设备因升级服务异常而不可用。
        }

        // 当前固件已正常运行到联网阶段，可以将 OTA 分区标记为有效，防止回滚。
        ota_->MarkCurrentVersionValid();
        /*
         * 没有 code/challenge 表示设备已激活或服务端不要求激活，本轮启动可以继续。
         * 仍需要保留本次响应写入 NVS 的 MQTT/WebSocket 配置供 InitializeProtocol() 使用。
         */
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // 无需激活时结束版本检查。
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // 若服务器要求绑定设备，向用户展示并播报激活码。
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // 激活在后台任务内阻塞重试，不占用主事件循环。
        /* 202/ESP_ERR_TIMEOUT 表示等待用户绑定，3 秒后重试；普通失败使用 10 秒降低服务压力。 */
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::InitializeProtocol() {
    /*
     * 版本响应已经把协议参数写入 NVS，此处只选择实现并统一注册上层回调。
     * MQTT 实现使用 MQTT 控制面与加密 UDP 音频数据面；WebSocket 在单连接中承载消息和音频。
     * 两者都实现 Protocol，因此后续对话状态机不感知具体传输方式。
     */
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    // 两种实现共用 Protocol 接口，上层对会话控制与音频收发保持无感。
    if (ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() {
        // 控制连接恢复后撤销旧的网络错误提示，设备保持当前业务状态。
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        // 回调线程只保存错误并置位，Alert 和状态转换由 Run() 执行。
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        // 只在 Speaking 状态接收 TTS 音频；过期包直接丢弃，避免下一轮会话播放残留语音。
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        // 实时会话期间提高 CPU/网络性能；采样率不一致时仍工作，但重采样可能降低音质。
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    
    protocol_->OnAudioChannelClosed([this, &board]() {
        // 网络回调先恢复电源策略，再投递 UI 与状态收尾，避免跨任务操作显示对象。
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        /*
         * 服务端 JSON 消息路由：
         * - tts：控制 Speaking 生命周期并显示逐句文本；
         * - stt：显示用户语音识别结果；
         * - llm：更新模型选择的表情；
         * - mcp：交给本地 MCP 工具服务器执行；
         * - system/alert/custom：系统控制、统一告警和可选扩展消息。
         * 涉及 UI 或状态机的动作通过 Schedule() 回到主任务。
         */
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                // 服务端确认即将下发 TTS 后才进入 Speaking，防止空等待状态。
                Schedule([this]() {
                    if (conversation_suspended_.load()) {
                        return;
                    }
                    aborted_ = false;
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                // 手动监听模式一次播报后结束；自动/实时模式回到 Listening 继续多轮对话。
                Schedule([this]() {
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                // 文本通常早于对应音频到达，用于即时更新聊天气泡。
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            // STT 文本属于用户角色，与 TTS 的 assistant 角色分开显示。
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            // MCP payload 由 McpServer 校验方法名、参数并产生工具调用结果。
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // OTA/配置更新完成后，服务端可请求设备有序重启使新配置生效。
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    
    // Start() 启动协议控制连接；实时音频通道要等唤醒或按键请求时再打开。
    protocol_->Start();
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    // 数字到内置 OGG 资源的映射，使无屏或看不清屏幕的用户也能听到设备绑定码。
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // 激活提示音解码约占 9 KB SRAM，因此先完整播放，再逐位播报激活码。
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    // 告警是 UI 的原子组合：顶部状态、中央表情、消息文本和可选提示音保持同步。
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    // 只在 Idle 清除告警，避免覆盖 Listening/Speaking 等状态正在展示的业务界面。
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    // 公开入口可能由按键任务调用，因此只产生事件，不在调用者任务中操作协议或状态机。
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT);
}

void Application::StartListening() {
    // 长按说话等交互使用显式开始/停止事件，对应 manual-stop 监听模式。
    xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING);
}

void Application::StopListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

void Application::SuspendConversation() {
    /*
     * WebRTC 门铃通话与助手共享 codec。挂起采用“两阶段”处理：原子门闩立即阻止新会话，
     * 然后由主任务中止 TTS、关闭协议通道并收敛到 Idle，确保外设交接不存在竞态窗口。
     */
    // 先立刻设置门闩，再异步拆对话链路；否则投递任务尚未执行的窗口里仍可能触发新对话。
    conversation_suspended_.store(true);
    Schedule([this]() {
        // 先通知服务端中止正在进行的 TTS。
        if (GetDeviceState() == kDeviceStateSpeaking) {
            AbortSpeaking(kAbortReasonNone);
        }
        // 关闭协议音频通道；OnAudioChannelClosed 会回到 Idle 并恢复低功耗模式。
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // 某些尚未真正打开通道的状态不会触发 OnAudioChannelClosed，这里兜底拉回 Idle。
        auto state = GetDeviceState();
        if (state == kDeviceStateListening ||
            state == kDeviceStateSpeaking ||
            state == kDeviceStateConnecting) {
            SetDeviceState(kDeviceStateIdle);
        }
    });
}

void Application::ResumeConversation() {
    conversation_suspended_.store(false);
}

void Application::HandleToggleChatEvent() {
    /*
     * 单击按键根据当前状态复用为多种动作：
     * Activating -> 取消等待；WifiConfiguring <-> AudioTesting；Idle -> 开始对话；
     * Speaking -> 打断播报；Listening -> 关闭本轮对话。
     */
    if (conversation_suspended_.load()) {
        ESP_LOGI(TAG, "Conversation suspended; ignoring toggle chat");
        return;
    }
    auto state = GetDeviceState();

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        ListeningMode mode = GetDefaultListeningMode();
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // 先让主循环处理 Connecting 的 UI，再执行可能阻塞约 1 秒的建连。
            Schedule([this, mode]() {
                ContinueOpenAudioChannel(mode);
            });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        protocol_->CloseAudioChannel();
    }
}

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    /*
     * OpenAudioChannel() 可能执行 DNS/TLS/协议握手而短暂阻塞。入口先把状态切成 Connecting，
     * 再通过 Schedule() 延后一轮执行，让 UI 有机会先显示“连接中”。真正执行时再次校验状态，
     * 防止用户已取消会话却仍建立过期连接。
     */
    // 投递到真正执行之间状态可能已变化，过期任务必须丢弃。
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // 建连前切到性能模式，降低 TLS/信令握手延迟。
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            return;
        }
    }

    SetListeningMode(mode);
}

void Application::HandleStartListeningEvent() {
    // 显式开始监听使用 ManualStop：麦克风持续工作，直到收到 StopListening() 请求。
    if (conversation_suspended_.load()) {
        return;
    }
    auto state = GetDeviceState();

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // 先刷新 Connecting 界面，再执行阻塞式建连。
            Schedule([this]() {
                ContinueOpenAudioChannel(kListeningModeManualStop);
            });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    // 停止动作在音频测试态和云端监听态含义不同，分别关闭本地回环或通知服务端结束采集。
    auto state = GetDeviceState();
    
    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (protocol_) {
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleWakeWordDetectedEvent() {
    /*
     * AudioService 命中唤醒词后的状态分流：
     * - Idle：编码唤醒前后语音，必要时打开音频通道，再开始监听；
     * - Speaking/Listening：把唤醒视为打断并重新开始一轮；
     * - Activating：允许用户用唤醒词跳过等待。
     */
    if (conversation_suspended_.load()) {
        return;
    }
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (state == kDeviceStateIdle) {
        // 编码操作把 WakeWord 模块保存的预卷 PCM 转成可经 Protocol 发送的 Opus 包。
        audio_service_.EncodeWakeWord();
        auto wake_word = audio_service_.GetLastWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // 先处理 Connecting 的界面变化，再执行可能阻塞约 1 秒的 OpenAudioChannel()。
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // 通道已存在时无需重新握手，直接进入唤醒词处理。
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
        // 清空旧的待发送音频，避免下一轮监听把上一轮残留送给服务器。
        while (audio_service_.PopPacketFromSendQueue());

        if (state == kDeviceStateListening) {
            protocol_->SendStartListening(GetDefaultListeningMode());
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            // 唤醒词模块命中后会自行停止，需要显式重新开启。
            audio_service_.EnableWakeWordDetection(true);
        } else {
            // 等状态切到 Listening 后再播放提示音并恢复监听。
            play_popup_on_listening_ = true;
            SetListeningMode(GetDefaultListeningMode());
        }
    } else if (state == kDeviceStateActivating) {
        // 激活期间的唤醒用于取消当前激活等待并回到待机。
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    // 与按键建连相同，本函数是延迟执行阶段；先检查 Connecting 状态以淘汰过期任务。
    // 丢弃排队期间已经过期的建连任务。
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // 建连阶段使用性能模式，通道关闭后再由回调恢复低功耗。
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            audio_service_.EnableWakeWordDetection(true);
            return;
        }
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
    // 将触发唤醒前后的音频一并编码发送，给服务端提供更完整的语境。
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        protocol_->SendAudio(std::move(packet));
    }
    // 通知服务端本轮对话由哪个唤醒词触发。
    protocol_->SendWakeWordDetected(wake_word);
    SetListeningMode(GetDefaultListeningMode());
#else
    // 延迟到 Listening 后播放；此处直接播放会被 EnableVoiceProcessing() 中的
    // ResetDecoder() 清掉。
    play_popup_on_listening_ = true;
    SetListeningMode(GetDefaultListeningMode());
#endif
}

void Application::HandleStateChangedEvent() {
    /*
     * DeviceStateMachine 只记录合法状态，实际副作用集中在这里：刷新 LED/UI，启停语音处理、
     * 唤醒词检测和解码器。集中处理能保证无论状态由按键、网络还是服务端触发，策略都一致。
     */
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;

#ifdef CONFIG_ENABLE_VAD_BARGE_IN
    if (new_state != kDeviceStateSpeaking && barge_in_timer_handle_ != nullptr) {
        esp_timer_stop(barge_in_timer_handle_);
    }
#endif

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    
    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            // Idle 停止上行语音处理，但保持低功耗唤醒词检测，等待下一次交互。
            display->SetStatus(Lang::Strings::STANDBY);
            display->ClearChatMessages();   // 先清消息
            display->SetEmotion("neutral"); // 再设表情；微信模式会检查消息子节点数量
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateConnecting:
            // 连接阶段不改音频管线，直到协议确认通道已打开并进入 Listening。
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            // 首次进入或被唤醒词打断后，确保语音处理管线已经运行。
            if (play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning()) {
                // 自动停止模式先等播放队列排空，避免网络抖动导致 STOP 晚到时截断尾音。
                if (listening_mode_ == kListeningModeAutoStop) {
                    audio_service_.WaitForPlaybackQueueEmpty();
                }
                
                // 服务端确认开始监听后，本地才把麦克风数据送入处理管线。
                protocol_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(true);
            }

#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
            // 按 Kconfig 决定监听期间是否同时保持 AFE 唤醒词检测。
            audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
            // 默认监听期间关闭唤醒词检测，避免重复触发。
            audio_service_.EnableWakeWordDetection(false);
#endif
            
            // 必须在 EnableVoiceProcessing() 重置解码器之后播放，否则提示音会被清掉。
            if (play_popup_on_listening_) {
                play_popup_on_listening_ = false;
                audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            }
            break;
        case kDeviceStateSpeaking:
            // 非实时模式播放 TTS 时停止普通上行；实时 AEC 模式允许边播边听。
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
#ifdef CONFIG_ENABLE_VAD_BARGE_IN
                if (audio_service_.EnableBargeInDetection(true)) {
                    // 自由打断已覆盖唤醒词打断，避免同时运行两套 AFE 增加负载。
                    audio_service_.EnableWakeWordDetection(false);
                } else {
                    // 无参考通道时回退到原有的唤醒词打断。
                    audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
                }
#else
                // 播报期间只允许支持回声环境的 AFE 唤醒词检测。
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#endif
            }
            audio_service_.ResetDecoder();
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            // 其余状态的界面由对应流程单独维护。
            break;
    }
}

void Application::Schedule(std::function<void()>&& callback) {
    /*
     * 通用跨任务投递器。闭包先在 mutex_ 保护下入队，再置事件位；Run() 会整批移动队列并在
     * 无锁状态执行，因此闭包内部可再次 Schedule()，也不会长时间阻塞生产者任务。
     */
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    // 先入队再置位，保证主循环被唤醒时一定能取到任务。
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::AbortSpeaking(AbortReason reason) {
    // aborted_ 防止本轮剩余 TTS 被当作正常结束；同时向服务端发送明确的打断原因。
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    // 先保存模式再切状态，确保 HandleStateChangedEvent() 看到与新状态匹配的策略。
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

ListeningMode Application::GetDefaultListeningMode() const {
    // 无 AEC 时不能可靠边播边录，使用语音端点自动停止；具备 AEC 时采用实时全双工。
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

void Application::Reboot() {
    /*
     * 有序重启避免网络回调或 I2S DMA 在芯片复位窗口继续访问资源。这里不清 NVS，也不改变
     * 启动分区；若此前 OTA 已调用 esp_ota_set_boot_partition()，Bootloader 会进入新镜像。
     */
    ESP_LOGI(TAG, "Rebooting...");
    // 重启前先停网络音频和本地音频任务，避免外设仍在 DMA 时复位。
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    /*
     * 这是 Ota::Upgrade() 外层的业务事务：关闭实时会话 -> 显示升级提示 -> 切性能模式 ->
     * 停止 AudioService 释放 I2S/CPU/内存 -> 下载写入 -> 失败则恢复服务，成功则有序重启。
     * 因此 Ota 类可保持无 UI、无状态机，而手动升级和自动升级得到相同行为。
     */
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    // 升级会长期占用网络和 Flash，先关闭实时音频通道。
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Ota 回调不直接更新 LVGL，而是把格式化后的进度文本投递到主任务。
    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    });

    if (!upgrade_success) {
        // 升级失败时恢复音频与低功耗策略，设备继续运行而不是留在半停机状态。
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start(); // 恢复音频服务
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER); // 恢复低功耗策略
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // 新固件写入成功后立即重启进入新分区。
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // 留出时间显示成功提示
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    
    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // 先处理 Connecting 的 UI，再执行阻塞式建连。
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // 已连接时直接复用现有通道。
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (state == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // 只有状态机、网络音频和本地音频三者都空闲时才允许休眠。
    return true;
}

void Application::RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback) {
    mcp_broadcast_callback_ = std::move(callback);
}

void Application::SendMcpMessage(const std::string& payload) {
    // MCP 可能来自协议或本地广播任务，统一回主任务发送，保护 protocol_ 生命周期。
    Schedule([this, payload](){ 
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
        if (mcp_broadcast_callback_) {
            mcp_broadcast_callback_(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // AEC 能力会写入下一次 hello；关闭当前通道以便重新协商。
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::ResetProtocol() {
    Schedule([this]() {
        // protocol_ 只在主任务销毁，避免与网络回调并发释放。
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}

