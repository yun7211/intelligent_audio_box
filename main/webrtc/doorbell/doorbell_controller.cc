/* 门铃控制器。职责与线程约定见 doorbell_controller.h。 */
#include "doorbell_controller.h"
#include "doorbell_ui.h"
#include "door_lock.h"
#include "webrtc_call_service.h"

#include "application.h"
#include "settings.h"
#include <cJSON.h>
#include <esp_log.h>
#include <sdkconfig.h>

#include <cstdio>
#include <cstring>

#define TAG "Doorbell"

// 旧的三条纯文本控制命令，保留是为了兼容仍在用它们的对端。它们只驱动屏上显示，
// **一律不碰门锁 GPIO** —— 纯文本命令没有结构，也无法区分来源，不适合作为开门凭据。
#define CMD_RING        "RING"
#define CMD_OPEN_DOOR   "OPEN_DOOR"
#define CMD_DOOR_OPENED "DOOR_OPENED"

// 远程开门协议（设计文档 §4）。请求与应答都是 JSON 对象，只在 DataChannel 上收发：
//   请求 {"type":"open_door","request_id":"..."}
//   应答 {"type":"door_result","request_id":"...","status":"opened|busy|denied|error"}
#define MSG_TYPE_OPEN_DOOR   "open_door"
#define MSG_TYPE_DOOR_RESULT "door_result"
#define DOOR_STATUS_OPENED   "opened"
#define DOOR_STATUS_BUSY     "busy"
#define DOOR_STATUS_DENIED   "denied"
#define DOOR_STATUS_ERROR    "error"

// 先量长度再解析：cJSON 会为整条消息分配内存，让对端决定我们分配多少不是个好主意。
// 512 字节远大于协议里最长的合法消息，同时把畸形大包挡在解析器之前。
static constexpr size_t kMaxControlMsgLen = 512;
// request_id 只用来做请求-应答配对，64 字符够用。
static constexpr size_t kMaxRequestIdLen  = 64;

#define ROOM_SETTINGS_NS  "doorbell"
#define ROOM_SETTINGS_KEY "room_number"

// request_id 会被原样拼进应答 JSON，所以字符集必须收紧到"拼进去也不可能改变 JSON 结构"
// 的范围。这样就不需要在发送侧做转义 —— 少一处可能写错的转义代码，就少一个注入面。
static bool IsSafeRequestId(const char* id) {
    if (id == nullptr || *id == '\0') {
        return false;
    }
    size_t n = 0;
    for (const char* p = id; *p != '\0'; ++p, ++n) {
        if (n >= kMaxRequestIdLen) {
            return false;
        }
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

DoorbellController& DoorbellController::GetInstance() {
    static DoorbellController instance;
    return instance;
}

void DoorbellController::Enable() {
    if (enabled_) {
        return;   // 幂等：重复 enable 不该把回调重挂一遍
    }
    enabled_ = true;

    // 门锁先初始化好，这样第一条开门请求不必现场配置 GPIO。初始化失败只让开门不可用，
    // 不影响门铃与通话本身（设计文档 §9）—— 所以这里只记日志，不 return。
    if (!DoorLock::GetInstance().Init()) {
        ESP_LOGW(TAG, "Door lock unavailable; remote unlock will report error");
    }

#if defined(CONFIG_WEBRTC_SIGNALING_LOCAL_HTTP)
    // 这个组合是可用的，但门槛比另一条低，所以每次开门铃都要在串口上说一次：设备自带的
    // 局域网信令端点没有鉴权，同一个网里的任何主机都能成为对端，而对端拿得到
    // DataChannel —— 也就是拿得到开门的资格。DataChannel 那道边界防的是"往房间里注入
    // 消息"，防不住"完成握手当上对端"。选 AppRTC 至少还要先知道房间号。
    ESP_LOGW(TAG, "Local LAN signaling has no authentication: any host on this "
                  "network can become the peer and unlock the door");
#endif

    auto& ui = DoorbellUi::GetInstance();
    ui.OnRing([this]() { OnRingButton(); });
    ui.OnOpen([this]() { OnOpenButton(); });
#if defined(CONFIG_WEBRTC_SIGNALING_APPRTC_WS)
    // 房间号只有外部 AppRTC 信令模式才用得上（本地信令直接连设备 IP，没有房间概念）。
    // 先把持久化的房间号读出来交给通话服务，这样即使用户从没打开过房间号界面就直接呼叫，
    // 用的也是上次设定的那个房间。
    room_ = Settings(ROOM_SETTINGS_NS, true).GetString(ROOM_SETTINGS_KEY, "");
    WebrtcCallService::GetInstance().SetRoomId(room_);
    ui.OnRoomSetting([this]() { OnRoomSetting(); });
    ui.OnRoomConfirm([this](const std::string& room) { OnRoomConfirm(room); });
    ui.OnRoomCancel([this]() { OnRoomCancel(); });
#endif
    ui.Show();
#if defined(CONFIG_WEBRTC_SIGNALING_APPRTC_WS)
    // 必须在 Show() 之后：控件是 Show() 里创建的，之前调 SetRoomDisplay 无处可写。
    ui.SetRoomDisplay(room_);
#endif

    // 对端命令是在 webrtc 任务上到达的 -> 跳到主任务再处理。
    // cmd 按值拷进 c 再捕获：原引用属于 webrtc 任务的栈，lambda 跑起来时它已经没了。
    // via 必须一起带过去：它是"这条消息能不能开门"的依据，丢了就再也补不回来。
    WebrtcCallService::GetInstance().SetCustomDataHandler(
        [this](const std::string& cmd, WebrtcDataVia via) {
            std::string c = cmd;
            Application::GetInstance().Schedule([this, c, via]() { OnPeerCommand(c, via); });
        });

    // 通话状态 -> UI 状态文字。状态变化同样可能来自 webrtc 任务，所以也要跳线程 ——
    // 这正是 SetStateChangeHandler 在头文件里声明的那个约束（回调方自己负责线程跳转）。
    WebrtcCallService::GetInstance().SetStateChangeHandler([this](WebrtcCallState state) {
        Application::GetInstance().Schedule([this, state]() { OnCallStateChanged(state); });
    });

    ESP_LOGI(TAG, "Doorbell mode enabled");
}

void DoorbellController::Disable() {
    if (!enabled_) {
        return;
    }
    enabled_ = false;
    // 先挂断再摘回调：顺序反了的话，Stop() 触发的状态变化会打到已经被隐藏的 UI 上。
    if (WebrtcCallService::GetInstance().state() != WebrtcCallState::kIdle) {
        WebrtcCallService::GetInstance().Stop();
    }
    WebrtcCallService::GetInstance().SetCustomDataHandler(nullptr);
    WebrtcCallService::GetInstance().SetStateChangeHandler(nullptr);
    // 退出门铃模式时强制收锁：正在进行的脉冲要立刻结束。留一个高电平出去，等于门铃
    // 一关门就一直开着。
    DoorLock::GetInstance().ForceClose();
    DoorbellUi::GetInstance().Hide();
    ESP_LOGI(TAG, "Doorbell mode disabled");
}

void DoorbellController::OnRingButton() {
    // 从 LVGL 的事件回调里被调用 -> 真正的活儿全部丢到主任务上做。
    // 不在 LVGL 回调里直接跑 Start()：那会在 LVGL 的锁里做挂起助手、开引擎这类重活。
    Application::GetInstance().Schedule([this]() {
        auto& svc = WebrtcCallService::GetInstance();
        if (svc.state() == WebrtcCallState::kIdle) {
            DoorbellUi::GetInstance().SetStatus("呼叫中...");
            if (svc.Start()) {
                // RING 必须在 Start() 成功之后才发：失败时引擎没起来，发不出去。
                svc.SendCustomData(CMD_RING);
            } else {
                DoorbellUi::GetInstance().SetStatus("呼叫失败");
            }
        } else {
            // 同一个按钮兼作挂断键 —— 而且在语音发起的通话里，**它是唯一的挂断入口**
            // （MCP 的 stop 通话期间不可达，见 webrtc_mcp_tools.cc）。
            svc.Stop();
            DoorbellUi::GetInstance().SetStatus("门铃待机");
        }
    });
}

void DoorbellController::OnOpenButton() {
    Application::GetInstance().Schedule([this]() { DoOpenDoor(); });
}

void DoorbellController::OnRoomSetting() {
    // 从 LVGL 事件回调里被调用 -> 在主任务上打开输入界面。
    Application::GetInstance().Schedule([this]() {
        DoorbellUi::GetInstance().ShowRoomInput(room_);
    });
}

void DoorbellController::OnRoomConfirm(const std::string& room) {
    Application::GetInstance().Schedule([this, room]() {
        ApplyRoom(room);
        DoorbellUi::GetInstance().HideRoomInput();
    });
}

void DoorbellController::OnRoomCancel() {
    Application::GetInstance().Schedule([this]() {
        DoorbellUi::GetInstance().HideRoomInput();
    });
}

// 房间号的三处落地一次做完：NVS 持久化、通话服务、屏上显示。
// 集中在一个函数里，是为了不出现"存了但没生效"或"显示了但没存"的不一致状态。
void DoorbellController::ApplyRoom(const std::string& room) {
    room_ = room;
    Settings(ROOM_SETTINGS_NS, true).SetString(ROOM_SETTINGS_KEY, room);
    WebrtcCallService::GetInstance().SetRoomId(room);
    DoorbellUi::GetInstance().SetRoomDisplay(room);
    // 房间号现在是远程开门的唯一门槛，所以只报"设了/清了"，不把它打进日志。
    ESP_LOGI(TAG, "Room number %s", room.empty() ? "cleared" : "updated");
}

void DoorbellController::OnCallStateChanged(WebrtcCallState state) {
    // 通话一结束就强制收锁：脉冲本来会自己到点回落，但"通话没了锁还开着"这种状态
    // 一秒都不该存在。ForceClose() 是幂等的，没在开锁时调用它没有副作用。
    if (state == WebrtcCallState::kIdle) {
        DoorLock::GetInstance().ForceClose();
    }

    switch (state) {
        // kIdle 既是"待机"也是"刚失败/刚结束"的落点，所以必须再看 end_reason 才能说清
        // 到底发生了什么 —— "未接通"（ICE 压根没建起来）和用户主动挂断，对用户来说是
        // 完全不同的两件事，都显示"门铃待机"等于把诊断信息扔了。
        case WebrtcCallState::kIdle:
            switch (WebrtcCallService::GetInstance().end_reason()) {
                case WebrtcEndReason::kSetupError:
                    DoorbellUi::GetInstance().SetStatus("呼叫失败"); break;
                case WebrtcEndReason::kConnectFailed:
                    DoorbellUi::GetInstance().SetStatus("未接通"); break;
                case WebrtcEndReason::kPeerDisconnected:
                    DoorbellUi::GetInstance().SetStatus("对方已挂断"); break;
                default:
                    DoorbellUi::GetInstance().SetStatus("门铃待机"); break;
            }
            break;
        // 剩下五个状态与 WebrtcCallState 一一对应。刻意把 kWaiting（等人接）和
        // kConnecting（接了，正在打洞）显示成不同文字，用户就能自己判断是"没人理"
        // 还是"网络卡住了"。
        case WebrtcCallState::kStarting:   DoorbellUi::GetInstance().SetStatus("呼叫中..."); break;
        case WebrtcCallState::kWaiting:    DoorbellUi::GetInstance().SetStatus("等待接听..."); break;
        case WebrtcCallState::kConnecting: DoorbellUi::GetInstance().SetStatus("接通中..."); break;
        case WebrtcCallState::kInCall:     DoorbellUi::GetInstance().SetStatus("通话中"); break;
        case WebrtcCallState::kStopping:   DoorbellUi::GetInstance().SetStatus("挂断中..."); break;
    }
}

void DoorbellController::OnPeerCommand(const std::string& cmd, WebrtcDataVia via) {
    // 先按 JSON 协议试一次。解析成功即视为已处置，绝不再拿同一条消息去做旧文本命令的
    // 前缀匹配 —— 否则 {"type":"noop","x":"OPEN_DOOR..."} 这种消息会从两条路各走一遍。
    if (HandleControlJson(cmd, via)) {
        return;
    }

    // 旧的纯文本命令。用 rfind(x, 0) == 0 判前缀，而不是完全相等：命令后面允许带附加载荷。
    // 注意 CMD_OPEN_DOOR 走的是 DoOpenDoor()，只改屏上文字，**不驱动门锁 GPIO**。
    if (cmd.rfind(CMD_OPEN_DOOR, 0) == 0) {
        DoOpenDoor();
    } else if (cmd.rfind(CMD_RING, 0) == 0) {
        DoorbellUi::GetInstance().SetStatus("响铃中...");
    }
}

// 解析一条对端控制消息。返回 true = 这条消息已由 JSON 协议处置（含"格式合法但被拒"
// 和"超长直接丢弃"），调用方不应再做其他解释。
//
// 所有拒绝分支都**只记录原因，不记录消息内容** —— 这条通道上跑的是开门凭据，一旦
// 原样进了串口日志，日志就变成了凭据的副本。
bool DoorbellController::HandleControlJson(const std::string& msg, WebrtcDataVia via) {
    if (msg.size() > kMaxControlMsgLen) {
        ESP_LOGW(TAG, "Dropped oversized control message (%u bytes)", (unsigned)msg.size());
        return true;   // 已处置：超长包不该再有第二次机会被解释成任何命令
    }

    cJSON* root = cJSON_ParseWithLength(msg.data(), msg.size());
    if (root == nullptr) {
        return false;  // 不是 JSON —— 交回去按旧文本命令处理
    }
    if (!cJSON_IsObject(root)) {
        // 合法 JSON 但不是对象（数组、裸字符串……）。协议只认对象，直接丢。
        cJSON_Delete(root);
        return true;
    }

    const cJSON* type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type) || type->valuestring == nullptr) {
        ESP_LOGW(TAG, "Rejected control message: missing type");
        cJSON_Delete(root);
        return true;
    }

    if (std::strcmp(type->valuestring, MSG_TYPE_OPEN_DOOR) == 0) {
        const cJSON* rid = cJSON_GetObjectItem(root, "request_id");
        if (!cJSON_IsString(rid) || !IsSafeRequestId(rid->valuestring)) {
            // 没有合法 request_id 就无从配对应答，也不满足协议 —— 拒绝，且不回执
            // （连回给谁都说不清）。
            ESP_LOGW(TAG, "Rejected open_door: invalid request_id");
            cJSON_Delete(root);
            return true;
        }
        std::string request_id = rid->valuestring;
        cJSON_Delete(root);
        HandleOpenDoorRequest(request_id, via);
        return true;
    }

    // 未知 type：认得出是本协议的消息，但这个动作我们不支持。只报类型不报内容。
    ESP_LOGW(TAG, "Ignored control message of unsupported type");
    cJSON_Delete(root);
    return true;
}

// 唯一一处会驱动门锁的函数。三道闸门写在一起，是为了"能不能开门"这个判断只有一个
// 地方需要审：
//   1) 来源必须是 DataChannel（DTLS/SCTP，端到端加密，且只在通话建立后存在）
//   2) 通话必须处于 kInCall（不是"在拨"，也不是"刚挂"）
//   3) 控制通道必须仍然可写
// 三者缺一即拒绝，**没有任何回退路径**。
void DoorbellController::HandleOpenDoorRequest(const std::string& request_id, WebrtcDataVia via) {
    if (via != WebrtcDataVia::kDataChannel) {
        // 信令通道（明文房间广播）上的开门请求：连应答都不给。回一句 denied 也是在
        // 承认那条通道能和门对话，而它不能。
        ESP_LOGW(TAG, "Rejected open_door: not received over the DataChannel");
        return;
    }

    auto& svc = WebrtcCallService::GetInstance();
    if (svc.state() != WebrtcCallState::kInCall || !svc.data_channel_ready()) {
        // 到这里来源是可信的，所以要让对端知道为什么没开 —— 应答仍然只走 DataChannel。
        ESP_LOGW(TAG, "Rejected open_door: call not in progress");
        ReplyDoorResult(request_id, DOOR_STATUS_DENIED);
        return;
    }

    switch (DoorLock::GetInstance().Open()) {
        case DoorLockResult::kOpened:
            DoorbellUi::GetInstance().SetStatus("门已开");
            ESP_LOGI(TAG, "Remote unlock accepted");
            ReplyDoorResult(request_id, DOOR_STATUS_OPENED);
            break;
        case DoorLockResult::kBusy:
            // 上一次脉冲还没走完。刻意不重新计时，也不排队 —— 连点两下不该把门开得更久。
            ESP_LOGW(TAG, "Remote unlock rejected: pulse already running");
            ReplyDoorResult(request_id, DOOR_STATUS_BUSY);
            break;
        case DoorLockResult::kError:
            DoorbellUi::GetInstance().SetStatus("开门失败");
            ESP_LOGE(TAG, "Remote unlock failed at the GPIO layer");
            ReplyDoorResult(request_id, DOOR_STATUS_ERROR);
            break;
    }
}

// 应答只走 DataChannel。发不出去就算了：网页端有自己的超时提示，而在信令通道上补发
// 一次，等于把"门的状态"广播给房间里的所有人。
void DoorbellController::ReplyDoorResult(const std::string& request_id, const char* status) {
    // request_id 已经过 IsSafeRequestId() 过滤（[A-Za-z0-9_-]，≤64），status 是本文件里的
    // 字面量，所以这里直接拼字符串是安全的，不需要再走一次 cJSON 序列化。
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "{\"type\":\"%s\",\"request_id\":\"%s\",\"status\":\"%s\"}",
                  MSG_TYPE_DOOR_RESULT, request_id.c_str(), status);
    if (!WebrtcCallService::GetInstance().SendCustomDataViaDataChannel(buf)) {
        ESP_LOGW(TAG, "Failed to deliver door_result over the DataChannel");
    }
}

void DoorbellController::DoOpenDoor() {
    // 屏上的"开门"按钮，以及旧的纯文本 OPEN_DOOR 命令，都落在这里。**刻意只改状态文字，
    // 不驱动门锁 GPIO**：屏幕上的按钮谁都能按，纯文本命令又分不清来源，两者都不满足
    // 设计文档 §3 的信任条件。真实开门只有 HandleOpenDoorRequest() 一条路。
    DoorbellUi::GetInstance().SetStatus("门已开");
    // 只在通话中才回 DOOR_OPENED：没有通话时引擎已关，发送是无意义的空操作。
    if (WebrtcCallService::GetInstance().state() != WebrtcCallState::kIdle) {
        WebrtcCallService::GetInstance().SendCustomData(CMD_DOOR_OPENED);
    }
    ESP_LOGI(TAG, "Door opened (simulated, no GPIO)");
}
