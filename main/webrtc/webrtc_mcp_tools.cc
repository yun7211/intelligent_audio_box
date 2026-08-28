/* WebRTC 通话控制的 MCP 工具
 *
 * 把这条独立的 WebRTC 通话以 MCP 工具的形式暴露给语音助手，于是可以用语音或指令
 * 启动/挂断/查询。注册入口在 MCP 初始化路径里，由 `#if CONFIG_USE_WEBRTC_CALL` 门控。
 *
 * MCP 接口（见 main/mcp_server.h）：
 *   McpServer::GetInstance().AddTool(name, description, PropertyList, callback)
 *   callback: std::function<ReturnValue(const PropertyList&)>
 *   ReturnValue: std::variant<bool, int, std::string, cJSON*, ImageContent*>
 *
 * 注意：下面每个工具的 description 都是**给模型读的运行时字符串，不是注释**，所以一律
 * 保持英文原文 —— 它们要进 prompt，改语言等于改模型看到的输入。中文说明写在注释里。
 */
#include "webrtc_mcp_tools.h"

#include "sdkconfig.h"
#include "mcp_server.h"
#include "webrtc_call_service.h"
#if CONFIG_WEBRTC_DOORBELL_MODE
#include "doorbell/doorbell_controller.h"
#endif

#include <string>

void RegisterWebrtcMcpTools() {
    auto& mcp = McpServer::GetInstance();

    // start：发起通话。
    //
    // 这段 description 写得这么长、语气这么重，是在给一个**没修好的结构性缺陷**打补丁：
    // Start() 会调 SuspendConversation()，而它内部走到 CloseAudioChannel() —— 实现就是
    // websocket_.reset()。**MCP 工具本身就跑在这条 websocket 上**，于是通话一开始，
    // 助手就把自己的控制通道给关了，self.video_call.stop 在整个通话期间不可达。
    //
    // 所以只能把约束写进工具描述，让模型别开一个自己关不掉的通话：明确告诉它通话期间
    // 联系不上它、只能靠屏幕按钮或对端离开来结束、发起前要先告知用户怎么挂断、没屏幕的
    // 设备干脆不要发起。这是**损伤说明，不是优雅设计**；正确修法是解耦 MCP 控制通道与
    // 被挂起的对话音频通道，要动上游 Application 的生命周期，本期没做。
    //
    // 返回 false 是有价值的信号：说明通话压根没起来，助手仍然可达，可以接着调 get_stats
    // 看 last_end_reason 是不是 "setup_error"。
    mcp.AddTool(
        "self.video_call.start",
        "Start a standalone WebRTC audio/video call. This suspends the voice "
        "assistant conversation, which also closes the connection these MCP "
        "tools travel over: WHILE THE CALL IS UP YOU CANNOT BE REACHED, so you "
        "cannot hang it up yourself. The call ends only when the user presses "
        "the button on the device's doorbell screen or the remote peer leaves. "
        "Warn the user how to end the call before starting one, and do not "
        "start one if the device has no usable screen. Returns true if the call "
        "was started. False means it never started and you are still reachable "
        "— call self.video_call.get_stats, where last_end_reason "
        "\"setup_error\" indicates a local failure such as the network not "
        "being ready.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            return WebrtcCallService::GetInstance().Start();
        });

    // stop：挂断并恢复助手。
    // 工具本身是对的，问题在于**通话期间没人能调用它**（原因见上）。留着它是因为：
    // 门铃 UI 与外部调用方走的是同一个服务对象，而且一旦上面那个缺陷被修好，这里不用改。
    // 先判 kIdle 再动手，这样"没有通话可挂"会返回 false 而不是空跑一遍拆链路流程。
    mcp.AddTool(
        "self.video_call.stop",
        "Hang up the current WebRTC call and resume the voice assistant. "
        "Returns false if there was no call to hang up.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            auto& service = WebrtcCallService::GetInstance();
            if (service.state() == WebrtcCallState::kIdle) {
                return false;
            }
            service.Stop();
            return true;
        });

    // get_info：返回连接信息，内容随编译期选的信令模式而变 —— 外部 AppRTC 报信令基地址
    // 加"房间号配好了没"，本地信令报设备的调试页地址。**两种都不含凭证**：这个字符串会经
    // MCP 上云，而房间号是外部模式下远程开门的唯一门槛，不该出现在那里。
    mcp.AddTool(
        "self.video_call.get_info",
        "Get connection info for the current call as JSON. With external signaling "
        "this is the signaling base URL and whether a room number has been "
        "configured on the device; with device-hosted LAN signaling it is the URL of "
        "the device's own call page. No credentials are ever returned, and neither "
        "is the room number itself.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            return WebrtcCallService::GetInstance().GetInfo();
        });

    // get_state：当前状态机状态。
    mcp.AddTool(
        "self.video_call.get_state",
        "Get the current WebRTC call state: idle, starting, waiting, connecting, "
        "in_call, or stopping.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            return std::string(WebrtcCallService::GetInstance().GetStateName());
        });

    // get_stats：诊断数据。description 里刻意用了 "measured" 一词 —— 这里每个数都是
    // 测出来的，没有 RTT / 抖动 / 丢包，因为 esp_webrtc 不暴露 RTCP（详见
    // WebrtcCallService::GetStats() 的注释）。把 -1 的含义也写给模型，它才能正确解释
    // "该阶段从未到达"而不是当成 0。
    mcp.AddTool(
        "self.video_call.get_stats",
        "Get measured diagnostics for the last/current WebRTC call as JSON: "
        "connection setup latency split into wait_ms (until a peer joined) and "
        "ice_ms (NAT traversal), total setup_ms, talk_ms, why the last call "
        "ended (local_hangup / setup_error / connect_failed / "
        "peer_disconnected), and per-outcome call counters since boot. "
        "Durations of -1 mean that phase was never reached. Use this to explain "
        "why a call failed.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            return WebrtcCallService::GetInstance().GetStats();
        });

#if CONFIG_WEBRTC_DOORBELL_MODE
    // 门铃模式的开关。只在 CONFIG_WEBRTC_DOORBELL_MODE 开启时注册 —— 关闭时这两个工具
    // 根本不出现在工具列表里，模型不会去调一个不存在的功能。
    mcp.AddTool(
        "self.doorbell.enable",
        "Show the on-screen doorbell UI (ring + open-door buttons).",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            DoorbellController::GetInstance().Enable();
            return true;
        });
    mcp.AddTool(
        "self.doorbell.disable",
        "Hide the doorbell UI and end any active call.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            DoorbellController::GetInstance().Disable();
            return true;
        });
#endif
}
