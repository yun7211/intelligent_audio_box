/* 门铃控制器
 *
 * 把屏上的门铃界面和 WebRTC 通话服务缝在一起：
 *  - "门铃" 按钮   -> 启动/挂断通话，并向对端发 RING
 *  - "开门" 按钮   -> 屏上开锁提示（**不驱动 GPIO**，见下），向对端回 DOOR_OPENED
 *  - "设置" 按钮   -> 房间号输入（LVGL 键盘），存进 NVS
 *  - 对端命令路由到这里，并带上**来源通道**
 *  - 通话状态变化映射回屏上的状态文字
 *
 * 远程开门（真实 GPIO）与上面这些是两条路：只有从 **DataChannel** 进来、且当前正在
 * 通话中的 `open_door` JSON 才会驱动门锁。屏上按钮、信令通道上的命令、以及任何解析不了
 * 的消息都不会碰 GPIO —— 判定集中在 HandleOpenDoorRequest() 一个函数里，方便审。
 *
 * 只在 CONFIG_WEBRTC_DOORBELL_MODE 开启时编译。该选项 depends on
 * !USE_EMOTE_MESSAGE_STYLE —— 门铃界面与表情动画风格**互斥**，两者都要占整块屏幕。
 *
 * 注释里标了每个私有方法**从哪个任务被调用**，这不是啰嗦：本类同时被 LVGL 任务、
 * 主任务和 WebRTC 事件任务碰到，谁在哪个上下文里跑决定了能不能直接动 LVGL 对象。
 */
#ifndef WEBRTC_DOORBELL_CONTROLLER_H
#define WEBRTC_DOORBELL_CONTROLLER_H

#include <string>

#include "webrtc_call_service.h"

class DoorbellController {
public:
    static DoorbellController& GetInstance();

    void Enable();    // 显示 UI，挂上按钮 / 对端命令 / 状态变化三类回调
    void Disable();   // 隐藏 UI，结束任何进行中的通话，并强制关锁
    bool enabled() const { return enabled_; }

private:
    DoorbellController() = default;
    void OnRingButton();                        // 来自 UI（LVGL 任务）
    void OnOpenButton();                        // 来自 UI（LVGL 任务）
    void OnRoomSetting();                       // 来自 UI（LVGL 任务）：打开房间号输入
    void OnRoomConfirm(const std::string& room);// 来自 UI（LVGL 任务）：保存房间号
    void OnRoomCancel();                        // 来自 UI（LVGL 任务）：取消输入
    void OnPeerCommand(const std::string& cmd, WebrtcDataVia via);  // 对端（已跳到主任务）
    void OnCallStateChanged(WebrtcCallState state);  // 来自通话服务（已跳到主任务）
    void DoOpenDoor();                          // 屏上开锁提示（不驱动 GPIO）
    void ApplyRoom(const std::string& room);    // 持久化 + 同步房间号到 UI 与服务

    // 远程开门协议（主任务）。HandleControlJson 返回 true 表示这条消息已被 JSON 协议
    // 处置完毕，调用方不应再拿它去做旧文本命令的前缀匹配。
    bool HandleControlJson(const std::string& msg, WebrtcDataVia via);
    void HandleOpenDoorRequest(const std::string& request_id, WebrtcDataVia via);
    void ReplyDoorResult(const std::string& request_id, const char* status);

    bool enabled_ = false;
    std::string room_;                          // 当前房间号的缓存（空 = 未设置）
};

#endif // WEBRTC_DOORBELL_CONTROLLER_H
