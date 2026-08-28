/* 门铃屏幕（LVGL）
 *
 * 一整屏的门铃界面：顶部一行状态文字 +「门铃」「开门」两个大按钮，门铃模式激活时盖在
 * 助手界面之上。另带一个房间号输入界面（LVGL 键盘），用于设置 AppRTC 通话房间。
 * 所有 LVGL 访问都在小智的显示锁（DisplayLockGuard）里做 —— LVGL 不是线程安全的，
 * 而本类会被 LVGL 任务和主任务同时碰到。
 * 只在 CONFIG_WEBRTC_DOORBELL_MODE 开启时编译（该选项隐含板子有 LVGL 显示屏）。
 *
 * 注意：两个界面都是**独立屏**（lv_obj_create(NULL)），不挂在当前活动屏下面，所以
 * 拿不到活动屏的主题字体 —— 默认字体没有中文字形，屏上所有中文会变成方块。
 * Show() 里显式把主题的中文字体设上去，就是为了解决这个问题。
 */
#ifndef WEBRTC_DOORBELL_UI_H
#define WEBRTC_DOORBELL_UI_H

#include <string>
#include <functional>
#include <lvgl.h>

class DoorbellUi {
public:
    static DoorbellUi& GetInstance();

    void Show();                                // 创建并加载门铃屏
    void Hide();                                // 恢复到进入前的那一屏
    void SetStatus(const std::string& text);    // 刷新状态行（内部自己加锁，调用方不用管）
    void SetRoomDisplay(const std::string& room);    // 刷新主屏上的房间号标签
    void ShowRoomInput(const std::string& current);  // 打开房间号输入界面（带键盘）
    void HideRoomInput();                            // 关掉输入界面，退回门铃主屏

    // 五个回调都由 DoorbellController 挂上。回调是在 **LVGL 任务**里被调用的，所以
    // 实现方必须自己跳回主任务再干重活（见 doorbell_controller.cc）。
    void OnRing(std::function<void()> cb) { on_ring_ = cb; }
    void OnOpen(std::function<void()> cb) { on_open_ = cb; }
    void OnRoomSetting(std::function<void()> cb) { on_room_setting_ = cb; }
    void OnRoomConfirm(std::function<void(const std::string&)> cb) { on_room_confirm_ = cb; }
    void OnRoomCancel(std::function<void()> cb) { on_room_cancel_ = cb; }
    bool visible() const { return doorbell_screen_ != nullptr; }

private:
    DoorbellUi() = default;
    // LVGL 的事件回调必须是普通函数指针，拿不到 this，所以统一用静态函数 +
    // lv_obj_add_event_cb 的 user_data 传实例，再转发到成员回调上。
    static void RingEventCb(lv_event_t* e);
    static void OpenEventCb(lv_event_t* e);
    static void RoomSettingEventCb(lv_event_t* e);
    static void RoomConfirmEventCb(lv_event_t* e);
    static void RoomCancelEventCb(lv_event_t* e);

    lv_obj_t* doorbell_screen_ = nullptr;
    lv_obj_t* saved_screen_ = nullptr;          // 进入门铃前的活动屏，Hide() 时要还回去
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* room_label_ = nullptr;            // 主屏上显示的房间号
    lv_obj_t* room_input_screen_ = nullptr;     // 房间号输入界面（用到才创建）
    lv_obj_t* room_textarea_ = nullptr;         // 输入框，确认时从这里取文本
    std::function<void()> on_ring_;
    std::function<void()> on_open_;
    std::function<void()> on_room_setting_;
    std::function<void(const std::string&)> on_room_confirm_;
    std::function<void()> on_room_cancel_;
};

#endif // WEBRTC_DOORBELL_UI_H
