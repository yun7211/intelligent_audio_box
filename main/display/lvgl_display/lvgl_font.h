#pragma once

#include <lvgl.h>


// LVGL 字体所有权抽象；内置字体不释放，CBin 动态字体在析构时释放解析结果。
class LvglFont {
public:
    virtual const lv_font_t* font() const = 0;
    virtual ~LvglFont() = default;
};

// 编译期内置字体，仅借用指针。
class LvglBuiltInFont : public LvglFont {
public:
    LvglBuiltInFont(const lv_font_t* font) : font_(font) {}
    virtual const lv_font_t* font() const override { return font_; }

private:
    const lv_font_t* font_;
};


class LvglCBinFont : public LvglFont {
public:
    LvglCBinFont(void* data);
    virtual ~LvglCBinFont();
    virtual const lv_font_t* font() const override { return font_; }

private:
    lv_font_t* font_;
};
