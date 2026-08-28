/* 门铃屏幕（LVGL）。职责、线程约定与独立屏字体问题见 doorbell_ui.h。 */
#include "doorbell_ui.h"

#include "board.h"
#include "display.h"
#include "lvgl_theme.h"

#include <esp_log.h>
#include <sdkconfig.h>

#define TAG "DoorbellUi"

DoorbellUi& DoorbellUi::GetInstance() {
    static DoorbellUi instance;
    return instance;
}

// 独立屏（lv_obj_create(NULL)）不挂在活动屏下面，样式不会从活动屏继承下来，于是拿不到
// 主题配的中文字体；而 LVGL 内置默认字体**没有中文字形**，本文件里所有中文标签都会变成
// 方块。所以显式把主题字体设到屏根节点上，靠 LVGL 的样式继承覆盖它下面所有子控件。
// display 或 theme 取不到就直接返回：字体不对只是难看，不该因此不显示界面。
static void ApplyThemeFont(Display* display, lv_obj_t* screen) {
    if (display == nullptr || screen == nullptr) {
        return;
    }
    auto* theme = static_cast<LvglTheme*>(display->GetTheme());
    if (theme == nullptr || theme->text_font() == nullptr) {
        return;
    }
    lv_obj_set_style_text_font(screen, theme->text_font()->font(), 0);
}

// 下面五个静态回调都是同一个模式：从 user_data 取回实例，判空后转发给成员回调。
// 判空不是多余的 —— Disable() 会把回调摘成 nullptr，而此刻屏上可能还有一次点击在路上。
void DoorbellUi::RingEventCb(lv_event_t* e) {
    auto* self = static_cast<DoorbellUi*>(lv_event_get_user_data(e));
    if (self && self->on_ring_) {
        self->on_ring_();
    }
}

void DoorbellUi::OpenEventCb(lv_event_t* e) {
    auto* self = static_cast<DoorbellUi*>(lv_event_get_user_data(e));
    if (self && self->on_open_) {
        self->on_open_();
    }
}

void DoorbellUi::RoomSettingEventCb(lv_event_t* e) {
    auto* self = static_cast<DoorbellUi*>(lv_event_get_user_data(e));
    if (self && self->on_room_setting_) {
        self->on_room_setting_();
    }
}

void DoorbellUi::RoomConfirmEventCb(lv_event_t* e) {
    auto* self = static_cast<DoorbellUi*>(lv_event_get_user_data(e));
    // 这里多判一次 room_textarea_：确认按钮和输入框同生同死，但万一界面已被 Hide()
    // 拆掉而事件还在队列里，从空指针取文本会直接崩。
    if (self && self->on_room_confirm_ && self->room_textarea_ != nullptr) {
        std::string room = lv_textarea_get_text(self->room_textarea_);
        self->on_room_confirm_(room);
    }
}

void DoorbellUi::RoomCancelEventCb(lv_event_t* e) {
    auto* self = static_cast<DoorbellUi*>(lv_event_get_user_data(e));
    if (self && self->on_room_cancel_) {
        self->on_room_cancel_();
    }
}

void DoorbellUi::Show() {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard lock(display);
    if (doorbell_screen_ != nullptr) {
        return;  // 已经显示了。幂等，重复 Show() 不该再造一屏出来
    }

    // 先把当前活动屏记下来再换屏，Hide() 才有得还 —— 助手界面不属于本类，不能销毁它。
    saved_screen_ = lv_screen_active();
    doorbell_screen_ = lv_obj_create(NULL);
    // 必须在创建子控件**之前**设字体：靠继承生效，后设的话已建好的标签不会重新排版。
    ApplyThemeFont(display, doorbell_screen_);

    status_label_ = lv_label_create(doorbell_screen_);
    lv_label_set_text(status_label_, "门铃待机");
    lv_obj_align(status_label_, LV_ALIGN_TOP_MID, 0, 8);

    // 房间号显示 + 「设置」按钮，只在外部 AppRTC 信令模式下创建：那时房间号是设备与
    // 浏览器碰头的唯一约定。本地信令模式没有房间的概念（浏览器直接开设备 IP），建出来
    // 是两个永远不会被用到的控件，还占掉 320x240 屏上本就紧张的一行。
#if defined(CONFIG_WEBRTC_SIGNALING_APPRTC_WS)
    room_label_ = lv_label_create(doorbell_screen_);
    lv_label_set_text(room_label_, "房间: 未设置");
    lv_obj_align(room_label_, LV_ALIGN_TOP_LEFT, 16, 40);

    lv_obj_t* room_btn = lv_button_create(doorbell_screen_);
    lv_obj_set_size(room_btn, 64, 32);
    lv_obj_align(room_btn, LV_ALIGN_TOP_RIGHT, -16, 40);
    lv_obj_add_event_cb(room_btn, RoomSettingEventCb, LV_EVENT_CLICKED, this);
    lv_obj_t* room_btn_lbl = lv_label_create(room_btn);
    lv_label_set_text(room_btn_lbl, "设置");
    lv_obj_center(room_btn_lbl);
#endif

    // 两个主按钮做到 130x64：门铃是手指在门口按的，不是鼠标点的，按键必须够大。
    // 左右各偏 75px 是为了在 320px 宽的屏上留出中缝，别贴在一起误触。
    // 局部变量不存成员：建完就交给 LVGL 的父子树管理，Hide() 里删父节点会一起删掉。
    lv_obj_t* ring_btn = lv_button_create(doorbell_screen_);
    lv_obj_set_size(ring_btn, 130, 64);
    lv_obj_align(ring_btn, LV_ALIGN_CENTER, -75, 30);
    lv_obj_add_event_cb(ring_btn, RingEventCb, LV_EVENT_CLICKED, this);
    lv_obj_t* ring_lbl = lv_label_create(ring_btn);
    lv_label_set_text(ring_lbl, "门铃");
    lv_obj_center(ring_lbl);

    lv_obj_t* open_btn = lv_button_create(doorbell_screen_);
    lv_obj_set_size(open_btn, 130, 64);
    lv_obj_align(open_btn, LV_ALIGN_CENTER, 75, 30);
    lv_obj_add_event_cb(open_btn, OpenEventCb, LV_EVENT_CLICKED, this);
    lv_obj_t* open_lbl = lv_label_create(open_btn);
    lv_label_set_text(open_lbl, "开门");
    lv_obj_center(open_lbl);

    lv_screen_load(doorbell_screen_);
    ESP_LOGI(TAG, "Doorbell UI shown");
}

void DoorbellUi::Hide() {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard lock(display);
    if (doorbell_screen_ == nullptr) {
        return;
    }
    // 房间号界面还开着就顺手拆掉：Disable() 可能在任何时刻进来，不能把它留成孤儿屏。
    //
    // 已知短板：这里是先 del 再 load 下面那一屏。如果房间输入界面此刻正是活动屏，
    // 中间就存在一个"活动屏已被释放"的窗口。更稳的写法是像下面 doorbell_screen_ 一样
    // 先 lv_screen_load 换走、再 lv_obj_del，两处顺序保持一致。
    if (room_input_screen_ != nullptr) {
        lv_obj_del(room_input_screen_);
        room_input_screen_ = nullptr;
        room_textarea_ = nullptr;
    }
    // 先切回助手界面，再删自己这一屏 —— 反过来就是在删当前活动屏。
    if (saved_screen_ != nullptr) {
        lv_screen_load(saved_screen_);
    }
    lv_obj_del(doorbell_screen_);
    // 子控件由 lv_obj_del 递归释放，这里只需要把成员指针清干净：留着悬空指针的话，
    // 下一次 SetStatus() 会往已释放的内存里写。
    doorbell_screen_ = nullptr;
    status_label_ = nullptr;
    room_label_ = nullptr;
    saved_screen_ = nullptr;
    ESP_LOGI(TAG, "Doorbell UI hidden");
}

void DoorbellUi::SetStatus(const std::string& text) {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    // 状态文字是从主任务打进来的，而 LVGL 跑在自己的任务上 —— 必须持锁再动控件。
    DisplayLockGuard lock(display);
    // 界面没显示时静默丢弃，而不是报错：通话状态变化和门铃模式的开关是两条独立的
    // 时间线，"状态更新时界面刚好被隐藏了"是正常情况，不是异常。
    if (status_label_ != nullptr) {
        lv_label_set_text(status_label_, text.c_str());
    }
}

void DoorbellUi::SetRoomDisplay(const std::string& room) {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard lock(display);
    // 判空有两种成立的情形：界面已被 Hide() 掉而状态更新还在路上，以及本地信令模式下
    // 这个控件压根没被创建（见 Show()）。两种都静默返回，不算错误。
    if (room_label_ == nullptr) {
        return;
    }
    // 空房间号显示"未设置"而不是显示成"房间: "，否则用户分不清是没设还是显示坏了。
    std::string text = room.empty() ? "房间: 未设置" : ("房间: " + room);
    lv_label_set_text(room_label_, text.c_str());
}

void DoorbellUi::ShowRoomInput(const std::string& current) {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard lock(display);
    // 两个前置条件：门铃屏必须在（否则退出输入界面时无处可退），且不能已经开着一个。
    if (doorbell_screen_ == nullptr || room_input_screen_ != nullptr) {
        return;
    }

    // 输入界面用到才创建、关掉就销毁，不常驻。它带一整个 LVGL 键盘，是这套 UI 里最占
    // 内存的部分，而房间号一台设备一辈子只设几次，没必要为它长期占着内存。
    room_input_screen_ = lv_obj_create(NULL);
    ApplyThemeFont(display, room_input_screen_);

    lv_obj_t* title = lv_label_create(room_input_screen_);
    lv_label_set_text(title, "输入房间号");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    room_textarea_ = lv_textarea_create(room_input_screen_);
    lv_textarea_set_one_line(room_textarea_, true);
    lv_textarea_set_placeholder_text(room_textarea_, "房间号");
    // 把当前房间号预填进去：改一位数字不该让用户从头敲一遍。空串就不填，留占位符。
    if (!current.empty()) {
        lv_textarea_set_text(room_textarea_, current.c_str());
    }
    lv_obj_set_size(room_textarea_, 280, 30);
    lv_obj_align(room_textarea_, LV_ALIGN_TOP_MID, 0, 26);

    lv_obj_t* confirm_btn = lv_button_create(room_input_screen_);
    lv_obj_set_size(confirm_btn, 80, 30);
    lv_obj_align(confirm_btn, LV_ALIGN_TOP_LEFT, 16, 60);
    lv_obj_add_event_cb(confirm_btn, RoomConfirmEventCb, LV_EVENT_CLICKED, this);
    lv_obj_t* confirm_lbl = lv_label_create(confirm_btn);
    lv_label_set_text(confirm_lbl, "确认");
    lv_obj_center(confirm_lbl);

    lv_obj_t* cancel_btn = lv_button_create(room_input_screen_);
    lv_obj_set_size(cancel_btn, 80, 30);
    lv_obj_align(cancel_btn, LV_ALIGN_TOP_RIGHT, -16, 60);
    lv_obj_add_event_cb(cancel_btn, RoomCancelEventCb, LV_EVENT_CLICKED, this);
    lv_obj_t* cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "取消");
    lv_obj_center(cancel_lbl);

    lv_obj_t* kb = lv_keyboard_create(room_input_screen_);
    lv_keyboard_set_textarea(kb, room_textarea_);
    // 键盘必须被限制在标题/输入框/按钮下方那条区域里：LVGL 默认键盘会按屏幕比例铺开，
    // 在 320x240 的屏上直接压住上面那些控件，用户点不到「确认」。
    // 140px 是个起点，**没有按分辨率/字号实测调过** —— 换屏就得重新看一眼。
    lv_obj_set_width(kb, LV_PCT(100));
    lv_obj_set_height(kb, 140);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_screen_load(room_input_screen_);
    ESP_LOGI(TAG, "Room input shown");
}

void DoorbellUi::HideRoomInput() {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard lock(display);
    if (room_input_screen_ == nullptr) {
        return;
    }
    // 与 Hide() 里那段相反，这里顺序是对的：先切回门铃屏，再删输入界面。
    if (doorbell_screen_ != nullptr) {
        lv_screen_load(doorbell_screen_);
    }
    lv_obj_del(room_input_screen_);
    room_input_screen_ = nullptr;
    room_textarea_ = nullptr;   // 是 room_input_screen_ 的子控件，已被上面递归删掉
    ESP_LOGI(TAG, "Room input hidden");
}
