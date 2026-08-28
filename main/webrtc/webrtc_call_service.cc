/* WebrtcCallService —— 通话编排核心
 *
 * 把四件事串起来：本地/外部信令 + 音视频媒体桥 + 助手的挂起/恢复 + 一个小状态机。
 * 启停流程参考了 esp-webrtc-solution 的 doorbell_local/webrtc.c，但剥掉了门铃业务逻辑。
 *
 * 独占式通话模型：Start() 挂起助手的音频管线，把共享的 codec 移交给 WebRTC；
 * Stop() 与**任何一个失败分支**都经由 Cleanup() 把它还回去 —— 助手必须回得来。
 *
 * 可观测性：esp_webrtc 不向上层暴露任何 RTCP 统计（拿不到 RTT / 抖动 / 丢包），
 * 所以 GetStats() 报出去的每一个数都只来自这个状态机加一个单调时钟 —— 建连耗时拆成
 * 两个阶段、通话时长、按结局分类的计数。**这里没有一个估算值。**
 */
#include "webrtc_call_service.h"
#include "webrtc_media_bridge.h"

#include "esp_webrtc.h"
#include "esp_webrtc_defaults.h"
#include "esp_peer_default.h"
// 路径带 signaling/http_local/ 前缀是刻意的：那个目录只在 LOCAL_HTTP 选项下才进
// INCLUDE_DIRS，而 "webrtc" 始终在，所以这样写两种信令模式下都能解析到头文件。
#include "signaling/http_local/webrtc_http_server.h"

#include "board.h"
#include "audio_codec.h"
#include "camera.h"
#include "application.h"

#include "esp_netif.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include <cstdio>
#include <cstring>

#define TAG "WebrtcCall"

// ---- 辅助函数 ---------------------------------------------------------------

uint32_t WebrtcCallService::NowMs() {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool GetDeviceIp(char* out, size_t len) {
    esp_netif_t* netif = esp_netif_get_default_netif();
    if (netif == nullptr) {
        return false;
    }
    esp_netif_ip_info_t ip = {};
    if (esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0) {
        return false;
    }
    std::snprintf(out, len, IPSTR, IP2STR(&ip.ip));
    return true;
}

// 供 C 调用的事件回调；ctx 指向单例。
static int OnWebrtcEvent(esp_webrtc_event_t* event, void* ctx) {
    (void)ctx;
    switch (event->type) {
        case ESP_WEBRTC_EVENT_CONNECTING:
        case ESP_WEBRTC_EVENT_PAIRED:
            // CONNECTING = 正在收集 ICE 候选，PAIRED = SDP 已匹配。两者都只表示
            // "对端在那儿了，但媒体还没起来"；引擎要到 CONNECTED 才真正开始推流。
            ESP_LOGI(TAG, "Call negotiating (event %d)", (int)event->type);
            WebrtcCallService::GetInstance().NotifyConnecting();
            break;
        case ESP_WEBRTC_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Call connected");
            WebrtcCallService::GetInstance().NotifyConnected();
            break;
        case ESP_WEBRTC_EVENT_CONNECT_FAILED:
            // ICE 压根没建立起来：对端不可达、NAT 穿透失败、或没有可达的中继。
            // 这和"连上过又掉线"是两个不同的问题，不能混为一谈。
            ESP_LOGW(TAG, "Call setup failed (ICE never established)");
            WebrtcCallService::GetInstance().NotifyEnded(WebrtcEndReason::kConnectFailed);
            break;
        case ESP_WEBRTC_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Peer disconnected");
            WebrtcCallService::GetInstance().NotifyEnded(WebrtcEndReason::kPeerDisconnected);
            break;
        case ESP_WEBRTC_EVENT_DATA_CHANNEL_OPENED:
            ESP_LOGI(TAG, "Control DataChannel opened");
            WebrtcCallService::GetInstance().SetDataChannelReady(true);
            break;
        case ESP_WEBRTC_EVENT_DATA_CHANNEL_CLOSED:
        case ESP_WEBRTC_EVENT_DATA_CHANNEL_DISCONNECTED:
            ESP_LOGW(TAG, "Control DataChannel closed");
            WebrtcCallService::GetInstance().SetDataChannelReady(false);
            break;
        default:
            break;
    }
    return 0;
}

// 供 C 调用的自定义数据回调。**必须把来源 via 一起带上去**：远程开门只信 DataChannel，
// 业务层要能分辨这条消息是从加密的 SCTP 通道来的，还是从明文的房间信令来的。
static int OnCustomData(esp_webrtc_custom_data_via_t via, uint8_t* data, int size, void* ctx) {
    (void)ctx;
    if (data != nullptr && size > 0) {
        WebrtcDataVia origin = WebrtcDataVia::kUnknown;
        if (via == ESP_WEBRTC_CUSTOM_DATA_VIA_DATA_CHANNEL) {
            origin = WebrtcDataVia::kDataChannel;
        } else if (via == ESP_WEBRTC_CUSTOM_DATA_VIA_SIGNALING) {
            origin = WebrtcDataVia::kSignaling;
        }
        // 未知来源保持 kUnknown 而不是"就当是信令"：新增的传输方式默认落在不受信任的一侧。
        WebrtcCallService::GetInstance().HandleCustomData(
            std::string((const char*)data, size), origin);
    }
    return 0;
}

// ---- 生命周期 -------------------------------------------------------------

// 为一次通话尝试重开统计窗口。t_start_ 是 GetStats() 里所有时长的基准；其余打点在
// 对应事件触发前保持为 0，因此一次从未连通的尝试会把 wait/ice/setup 报成"不可用"
// （-1），而不是编造一个 0 出来。
//
// 这些打点**刻意不在拆链路时清零** —— 通话结束之后，GetStats() 还得能回答"上一通
// 为什么失败"。只有在这里、即新的尝试要覆盖旧答案时，才清零。
void WebrtcCallService::ResetCallTimeline() {
    t_start_.store(NowMs());
    t_paired_.store(0);
    t_connected_.store(0);
    last_talk_ms_.store(0);
    end_reason_.store(WebrtcEndReason::kNone);
    n_calls_.fetch_add(1);
}

bool WebrtcCallService::Start() {
    if (state_.load() != WebrtcCallState::kIdle) {
        // 这里刻意不调 ResetCallTimeline()：已经有一通电话在跑，那个统计窗口属于它，
        // 不属于这次被拒绝的请求 —— 否则会把正在进行的通话数据抹掉。
        ESP_LOGW(TAG, "Call already active (state %s)", GetStateName());
        return false;
    }

    char ip[16] = {};
    if (!GetDeviceIp(ip, sizeof(ip))) {
        // 没网是通话启动失败最常见的原因，所以要像其他 setup 失败一样归因 —— 否则
        // get_stats 会拿**上一通**电话的结局来回答，看起来就像这次尝试根本没发生过。
        // 此刻还没挂起任何东西，所以直接返回，不需要走 Cleanup()。
        ESP_LOGE(TAG, "Network not ready; cannot start call");
        ResetCallTimeline();
        end_reason_.store(WebrtcEndReason::kSetupError);
        return false;
    }

    SetState(WebrtcCallState::kStarting);
    ResetCallTimeline();
    data_channel_ready_.store(false);

    // 1) 取得 codec 的独占权：挂起助手。
    Application::GetInstance().GetAudioService().Suspend();
    // 不只停音频管线，连协议通道和对话状态机一起挂起，否则云端仍能在通话期间驱动
    // TTS/STT。代价见 SuspendConversation() 的注释：它会顺手切断 MCP 所在的连接。
    Application::GetInstance().SuspendConversation();

    // 2) 决定要不要视频：只有编译期开启**且**板子上真的有摄像头才开。
    //    esp-box-3 没有摄像头，运行时会走到 else 降级为纯音频，而不是报错。
    bool with_video = false;
#if CONFIG_WEBRTC_CALL_ENABLE_VIDEO
    Camera* camera = Board::GetInstance().GetCamera();
    if (camera != nullptr) {
        with_video = true;
        camera->Release();          // 把摄像头外设交给 esp_capture（单一属主）
        camera_released_ = true;
    }
#endif

    // 3) 在小智已初始化好的共享 codec handle 之上搭起媒体桥。
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    esp_codec_dev_handle_t rec  = codec ? codec->GetInputDevHandle()  : nullptr;
    esp_codec_dev_handle_t play = codec ? codec->GetOutputDevHandle() : nullptr;
    if (rec == nullptr || play == nullptr) {
        // 板子的 codec 不暴露 handle（基类默认返回 nullptr）—— 优雅失败，而不是崩溃。
        return FailStart("codec does not expose esp_codec_dev handles");
    }
    if (webrtc_media_bridge_build(rec, play, with_video) != 0) {
        return FailStart("failed to build media bridge");
    }

    // 4) 配置 WebRTC。
    esp_peer_default_cfg_t peer_default = {
        .agent_recv_timeout = 500,
    };
    esp_webrtc_cfg_t cfg = {};
    // PCMA/G711A 固定使用 8 kHz 单声道。通话期间媒体桥独占 codec：AEC 采集源读取
    // ES7210 的 MMR，播放端与采集端共同切到 8 kHz；Cleanup() 关闭 WebRTC 媒体后，
    // AudioService::Resume() 再按立创板原配置恢复 24 kHz 助手音频。
    cfg.peer_cfg.audio_info.codec       = ESP_PEER_AUDIO_CODEC_G711A;
    cfg.peer_cfg.audio_info.sample_rate = 8000;
    cfg.peer_cfg.audio_info.channel     = 1;
    cfg.peer_cfg.audio_dir              = ESP_PEER_MEDIA_DIR_SEND_RECV;
    if (webrtc_media_bridge_has_video()) {
        // 这里的分辨率/帧率必须与 webrtc_media_bridge.c 的 WEBRTC_VIDEO_* 保持一致，
        // 否则 SDP 里协商的参数和编码器实际吐出来的对不上。
        cfg.peer_cfg.video_info.codec  = ESP_PEER_VIDEO_CODEC_H264;
        cfg.peer_cfg.video_info.width  = 320;
        cfg.peer_cfg.video_info.height = 240;
        cfg.peer_cfg.video_info.fps    = 15;
        // 只上行：设备把画面推给浏览器，不接收对端视频（LCD 不显示对方）。
        cfg.peer_cfg.video_dir         = ESP_PEER_MEDIA_DIR_SEND_ONLY;
    } else {
        cfg.peer_cfg.video_dir         = ESP_PEER_MEDIA_DIR_NONE;
    }
    // 控制命令优先通过 WebRTC DataChannel 传输；对端尚未加入或通道尚未 OPENED 时，
    // SendCustomData() 会回退到信令通道，因此 RING 等建连前命令仍可发送。
    // 远程开门是唯一例外：它只走 DataChannel，见 SendCustomDataViaDataChannel()。
    cfg.peer_cfg.enable_data_channel    = true;
    // 刻意偏离参考实现之二：禁掉引擎自动重连，把重连策略留在应用层。若放任引擎静默重连，
    // 助手会一直停在挂起态，用户看到的是"设备没反应了"。现在对端一掉线就上报
    // kPeerDisconnected，走 Cleanup() 回到 idle 并恢复对话 —— 宁可让用户重新发起，
    // 也不要一个"看起来还在通话"的僵死状态。
    cfg.peer_cfg.no_auto_reconnect      = true;
    cfg.peer_cfg.extra_cfg              = &peer_default;
    cfg.peer_cfg.extra_size             = sizeof(peer_default);
    cfg.peer_cfg.ctx                    = this;
    cfg.peer_cfg.on_custom_data         = OnCustomData;
    cfg.peer_impl                       = esp_peer_get_default_impl();

    // 信令二选一，由 Kconfig 决定，两条路的媒体面完全一样 —— 区别只在"设备和浏览器
    // 靠什么碰头"，以及随之而来的门槛差异：AppRTC 至少要知道房间号，本地信令只要在同
    // 一个局域网里。远程开门对这条区别不敏感，因为它只认 DataChannel（见
    // SendCustomDataViaDataChannel），但"谁能成为对端"这件事本身就是门槛，见 Kconfig
    // 里 WEBRTC_SIGNALING_LOCAL_HTTP 的 help 与 DoorbellController::Enable() 的告警。
#if defined(CONFIG_WEBRTC_SIGNALING_APPRTC_WS)
    cfg.signaling_impl = esp_signaling_get_apprtc_impl();
    // 房间 URL 在运行时拼，这样设备上现填的房间号才能生效。
    // signal_url_ 是成员变量，为的是让这个 C 字符串在整通电话期间都保持有效 ——
    // 若用局部变量，esp_webrtc 拿到的就是一个已经析构的指针。
    signal_url_ = CONFIG_WEBRTC_SIGNALING_URL;
    if (!room_id_.empty()) {
        if (signal_url_.back() != '/') {
            signal_url_ += '/';
        }
        signal_url_ += room_id_;
    }
    cfg.signaling_cfg.signal_url = (char*)signal_url_.c_str();
#else
    cfg.signaling_impl           = esp_signaling_get_http_impl();
    cfg.signaling_cfg.signal_url = nullptr;  // 本地 HTTPS 信令自己起服务，不需要 URL
#endif

    // 5) 打开引擎 → 挂上媒体提供者与事件回调 → 启动。
    if (esp_webrtc_open(&cfg, (esp_webrtc_handle_t*)&webrtc_handle_) != 0) {
        return FailStart("esp_webrtc_open failed");
    }
    esp_webrtc_media_provider_t provider = {};
    webrtc_media_bridge_get_provider(&provider);
    esp_webrtc_set_media_provider((esp_webrtc_handle_t)webrtc_handle_, &provider);
    esp_webrtc_set_event_handler((esp_webrtc_handle_t)webrtc_handle_, OnWebrtcEvent, this);
    esp_webrtc_enable_peer_connection((esp_webrtc_handle_t)webrtc_handle_, true);  // 对端加入即自动建连

    if (esp_webrtc_start((esp_webrtc_handle_t)webrtc_handle_) != 0) {
        return FailStart("esp_webrtc_start failed");
    }

    SetState(WebrtcCallState::kWaiting);
#if defined(CONFIG_WEBRTC_SIGNALING_APPRTC_WS)
    // 刻意不打印信令 URL 或房间号：房间号是 AppRTC 模式下远程开门的唯一门槛，串口日志
    // 会被转贴到 issue 和聊天里，不该顺手把它带出去。
    ESP_LOGI(TAG, "Call started; waiting for the browser peer to join");
#else
    // 本地信令模式下打印地址是安全的、而且是必要的：那是一个局域网 IP，用户得知道往
    // 哪儿开浏览器，而这条 URL 本身不含任何凭证 —— 门槛是"在同一个网里"，不是这个字符串。
    ESP_LOGI(TAG, "Call started; open https://%s/webrtc/test", ip);
#endif
    return true;
}

void WebrtcCallService::Stop() {
    if (state_.load() == WebrtcCallState::kIdle) {
        return;
    }
    // 只有在引擎还没告诉我们通话为何结束时，才把它归因给用户 —— NotifyEnded() 会把
    // Stop() 调度到主任务上，所以引擎主动拆链路走的也是这同一条路径。
    WebrtcEndReason none = WebrtcEndReason::kNone;
    end_reason_.compare_exchange_strong(none, WebrtcEndReason::kLocalHangup);

    // 通话时长在这里冻结，而不是在 Cleanup() 里：上面那个 early return 保证这段代码
    // **每通电话恰好执行一次**；而 Cleanup() 是刻意设计成幂等的，且在那些根本没接通的
    // Start() 失败分支上也会跑。从未到达 CONNECTED 的通话保持 Start() 存进去的 0。
    uint32_t connected_at = t_connected_.load();
    if (connected_at != 0) {
        last_talk_ms_.store(NowMs() - connected_at);
    }

    SetState(WebrtcCallState::kStopping);
    Cleanup();
    ESP_LOGI(TAG, "Call stopped");
}

void WebrtcCallService::NotifyConnecting() {
    // 只记录**第一次**协商打点：CONNECTING 和 PAIRED 都会落到这里，而且 PAIRED 在
    // 重协商时还可能再来一次。用 CAS 保证只有第一次写得进去。
    uint32_t unset = 0;
    t_paired_.compare_exchange_strong(unset, NowMs());

    WebrtcCallState s = state_.load();
    if (s == WebrtcCallState::kStarting || s == WebrtcCallState::kWaiting) {
        SetState(WebrtcCallState::kConnecting);
    }
}

void WebrtcCallService::NotifyConnected() {
    WebrtcCallState s = state_.load();
    if (s == WebrtcCallState::kWaiting || s == WebrtcCallState::kStarting ||
        s == WebrtcCallState::kConnecting) {
        uint32_t now = NowMs();
        t_connected_.store(now);
        n_connected_.fetch_add(1);
        // 对端有可能直接到 CONNECTED，中间的 CONNECTING/PAIRED 一次都没看到（这两个
        // 都是 best-effort 通知）。这里兜底补上，好让 setup 时长拆成 wait/ice 两段之后
        // 加起来仍然对得上，而不是算出一个离谱的数。
        uint32_t unset = 0;
        t_paired_.compare_exchange_strong(unset, now);

        uint32_t start = t_start_.load();
        ESP_LOGI(TAG, "Call up: wait %ums, ice %ums, setup %ums",
                 (unsigned)(t_paired_.load() - start),
                 (unsigned)(now - t_paired_.load()),
                 (unsigned)(now - start));
        SetState(WebrtcCallState::kInCall);
    }
}

void WebrtcCallService::NotifyEnded(WebrtcEndReason reason) {
    if (reason == WebrtcEndReason::kConnectFailed) {
        n_connect_failed_.fetch_add(1);
    } else if (reason == WebrtcEndReason::kPeerDisconnected) {
        n_peer_disconnected_.fetch_add(1);
    }
    // 在拆链路之前先把原因锁住，免得随后的 Stop() 用 kLocalHangup 把它覆盖掉。
    // 一通电话只认第一个原因。
    WebrtcEndReason none = WebrtcEndReason::kNone;
    end_reason_.compare_exchange_strong(none, reason);

    // 拆链路一律调度回主任务执行：**绝不在 webrtc 事件任务里 close 引擎自己**
    // （那等于在回调里销毁回调的宿主）。
    Application::GetInstance().Schedule([]() { WebrtcCallService::GetInstance().Stop(); });
}

void WebrtcCallService::SetCustomDataHandler(
        std::function<void(const std::string&, WebrtcDataVia)> handler) {
    custom_data_handler_ = handler;
}

void WebrtcCallService::HandleCustomData(const std::string& data, WebrtcDataVia via) {
    // 在 webrtc 任务上被调用；要不要跳线程由 handler 自己负责（见头文件的约束说明）。
    // via 必须原样传下去，业务层据此决定敏感命令收不收。
    if (custom_data_handler_) {
        custom_data_handler_(data, via);
    }
}

void WebrtcCallService::SetDataChannelReady(bool ready) {
    data_channel_ready_.store(ready);
}

void WebrtcCallService::SendCustomData(const std::string& data) {
    if (webrtc_handle_ != nullptr && !data.empty()) {
        // 通话建立后走 DTLS/SCTP DataChannel；尚未建立时仍走原有信令路径。
        // DataChannel 写入失败也回退一次，避免关闭事件尚未到达时丢控制命令。
        int ret = -1;
        if (data_channel_ready_.load()) {
            ret = esp_webrtc_send_custom_data((esp_webrtc_handle_t)webrtc_handle_,
                                              ESP_WEBRTC_CUSTOM_DATA_VIA_DATA_CHANNEL,
                                              (uint8_t*)data.data(), (int)data.size());
        }
        if (ret != 0) {
            ret = esp_webrtc_send_custom_data((esp_webrtc_handle_t)webrtc_handle_,
                                              ESP_WEBRTC_CUSTOM_DATA_VIA_SIGNALING,
                                              (uint8_t*)data.data(), (int)data.size());
        }
        if (ret != 0) {
            ESP_LOGW(TAG, "Failed to send custom data, ret=%d", ret);
        }
    }
}

// 只走 DataChannel 的发送，**刻意没有信令回退**。远程开门的应答用它：如果一条开门请求
// 是从信令通道来的，我们连回执都不给 —— 回了就等于承认那条通道也能控制门。
bool WebrtcCallService::SendCustomDataViaDataChannel(const std::string& data) {
    if (webrtc_handle_ == nullptr || data.empty() || !data_channel_ready_.load()) {
        return false;
    }
    int ret = esp_webrtc_send_custom_data((esp_webrtc_handle_t)webrtc_handle_,
                                          ESP_WEBRTC_CUSTOM_DATA_VIA_DATA_CHANNEL,
                                          (uint8_t*)data.data(), (int)data.size());
    if (ret != 0) {
        ESP_LOGW(TAG, "DataChannel send failed, ret=%d", ret);
        return false;
    }
    return true;
}

// Start() 中所有"状态机已经离开 kIdle 之后"的失败分支的统一出口。先记下这是**本地**
// setup 失败 —— 这和 ICE 建连失败是两个不同的问题，不该被报成同一个 —— 再拆链路。
// 收敛到单一出口，是"无论哪条路失败助手都一定回得来"这个保证的实现方式。
bool WebrtcCallService::FailStart(const char* what) {
    ESP_LOGE(TAG, "Start failed: %s", what);
    WebrtcEndReason none = WebrtcEndReason::kNone;
    end_reason_.compare_exchange_strong(none, WebrtcEndReason::kSetupError);
    Cleanup();
    return false;
}

// 幂等的拆链路，Stop() 和 Start() 的每一个失败分支都走它。
// 幂等是必须的：它会从多条路径被调用（本地挂断、建连失败、对端断开）。
void WebrtcCallService::Cleanup() {
    data_channel_ready_.store(false);
    if (webrtc_handle_ != nullptr) {
        esp_webrtc_close((esp_webrtc_handle_t)webrtc_handle_);
        webrtc_handle_ = nullptr;
    }
    webrtc_media_bridge_destroy();
    Application::GetInstance().GetAudioService().Resume();
    // 恢复"能重新开始一段助手对话"的能力（状态机回到 Idle）。
    Application::GetInstance().ResumeConversation();
#if CONFIG_WEBRTC_CALL_ENABLE_VIDEO
    if (camera_released_) {
        Camera* camera = Board::GetInstance().GetCamera();
        if (camera != nullptr) {
            camera->Reacquire();   // 通话结束，把摄像头还给小智
        }
        camera_released_ = false;
    }
#endif
    SetState(WebrtcCallState::kIdle);
}

void WebrtcCallService::SetState(WebrtcCallState state) {
    if (state_.exchange(state) == state) {
        return;   // 状态没变，就不要发通知（否则 UI 会收到一串重复事件）
    }
    if (state_change_handler_) {
        state_change_handler_(state);
    }
}

std::string WebrtcCallService::GetInfo() {
    char ip[16] = {};
    if (!GetDeviceIp(ip, sizeof(ip))) {
        return "{\"error\":\"network not ready\"}";
    }
#if defined(CONFIG_WEBRTC_SIGNALING_APPRTC_WS)
    // 只报信令服务地址（配置项本身不含凭证）和房间是否已设置。**不报房间号** ——
    // 这个字符串会经 MCP 上云，房间号是开门的唯一门槛，不该出现在那里。
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "{\"mode\":\"external\",\"signaling\":\"%s\",\"room_configured\":%s}",
                  CONFIG_WEBRTC_SIGNALING_URL, room_id_.empty() ? "false" : "true");
#else
    // https 不是可选项：浏览器只在安全源下才授予 getUserMedia，纯 HTTP 页面拿不到麦克风。
    char buf[96];
    std::snprintf(buf, sizeof(buf),
                  "{\"mode\":\"local\",\"url\":\"https://%s/webrtc/test\"}", ip);
#endif
    return std::string(buf);
}

const char* WebrtcCallService::GetStateName() const {
    switch (state_.load()) {
        case WebrtcCallState::kIdle:       return "idle";
        case WebrtcCallState::kStarting:   return "starting";
        case WebrtcCallState::kWaiting:    return "waiting";
        case WebrtcCallState::kConnecting: return "connecting";
        case WebrtcCallState::kInCall:     return "in_call";
        case WebrtcCallState::kStopping:   return "stopping";
    }
    return "unknown";
}

static const char* EndReasonName(WebrtcEndReason reason) {
    switch (reason) {
        case WebrtcEndReason::kNone:             return "none";
        case WebrtcEndReason::kLocalHangup:      return "local_hangup";
        case WebrtcEndReason::kSetupError:       return "setup_error";
        case WebrtcEndReason::kConnectFailed:    return "connect_failed";
        case WebrtcEndReason::kPeerDisconnected: return "peer_disconnected";
    }
    return "unknown";
}

// 这里报出去的每一个数都是**测出来的，不是估的**：esp_webrtc 不暴露 RTCP 统计，
// 所以这份 JSON 里**刻意没有 RTT / 抖动 / 丢包率字段** —— 不是忘了写，是测不到就不报。
// 时长按 32 位毫秒时钟的无符号差值计算，跨越约 49 天的回绕后依然正确。
// -1 表示"该阶段从未到达"，这本身就是诊断信息（例如 ice_ms = -1 说明压根没人加入，
// 不必去查 NAT）。
std::string WebrtcCallService::GetStats() {
    uint32_t start     = t_start_.load();
    uint32_t paired    = t_paired_.load();
    uint32_t connected = t_connected_.load();

    // 分段：wait = 等到有对端出现花了多久，ice = 随后 NAT 穿透又花了多久。
    // 两段相加就是用户实际感知到的建连延迟。
    long wait_ms  = (start && paired)    ? (long)(uint32_t)(paired - start)    : -1;
    long ice_ms   = (paired && connected)? (long)(uint32_t)(connected - paired): -1;
    long setup_ms = (start && connected) ? (long)(uint32_t)(connected - start) : -1;

    // 通话进行中报实时时长；通话结束后报 Stop() 里冻结下来的那个值。
    long talk_ms;
    if (state_.load() == WebrtcCallState::kInCall && connected != 0) {
        talk_ms = (long)(uint32_t)(NowMs() - connected);
    } else {
        talk_ms = (long)last_talk_ms_.load();
    }

    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "{\"state\":\"%s\",\"calls\":%u,\"connected\":%u,"
                  "\"connect_failed\":%u,\"peer_disconnected\":%u,"
                  "\"last_end_reason\":\"%s\","
                  "\"wait_ms\":%ld,\"ice_ms\":%ld,\"setup_ms\":%ld,\"talk_ms\":%ld}",
                  GetStateName(),
                  (unsigned)n_calls_.load(),
                  (unsigned)n_connected_.load(),
                  (unsigned)n_connect_failed_.load(),
                  (unsigned)n_peer_disconnected_.load(),
                  EndReasonName(end_reason_.load()),
                  wait_ms, ice_ms, setup_ms, talk_ms);
    return std::string(buf);
}
