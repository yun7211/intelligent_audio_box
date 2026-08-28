#include "lvgl_theme.h"

// 主题对象保存 LVGL 字体、颜色和图片资源；管理器按名称供 LCD 界面切换主题。
LvglTheme::LvglTheme(const std::string& name) : Theme(name) {
}

lv_color_t LvglTheme::ParseColor(const std::string& color) {
    if (color.find("#") == 0) {
        // 将网页格式 #RRGGBB 拆成 LVGL 的 RGB 颜色；无效格式回退为黑色。
        uint8_t r = strtol(color.substr(1, 2).c_str(), nullptr, 16);
        uint8_t g = strtol(color.substr(3, 2).c_str(), nullptr, 16);
        uint8_t b = strtol(color.substr(5, 2).c_str(), nullptr, 16);
        return lv_color_make(r, g, b);
    }
    return lv_color_black();
}

LvglThemeManager::LvglThemeManager() {
}

LvglTheme* LvglThemeManager::GetTheme(const std::string& theme_name) {
    auto it = themes_.find(theme_name);
    if (it != themes_.end()) {
        return it->second;
    }
    return nullptr;
}

void LvglThemeManager::RegisterTheme(const std::string& theme_name, LvglTheme* theme) {
    themes_[theme_name] = theme;
}
