#include "lvgl_font.h"
#include <cbin_font.h>

// cbin_font_create 直接从资源分区中的二进制字体创建 LVGL 字体对象。
// 本包装类负责在析构时归还解析器分配的资源。
LvglCBinFont::LvglCBinFont(void* data) {
    font_ = cbin_font_create(static_cast<uint8_t*>(data));
}

LvglCBinFont::~LvglCBinFont() {
    if (font_ != nullptr) {
        cbin_font_delete(font_);
    }
}
