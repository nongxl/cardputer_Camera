#ifndef FILTER_MANAGER_H
#define FILTER_MANAGER_H

#include <Arduino.h>
#include <M5Cardputer.h>
#include "Global.h"

// 滤镜模式枚举
enum FilterMode {
    FILTER_NONE     = 0,
    FILTER_GAMEBOY  = 1,  // 按键7 - GameBoy 经典绿 (纯 LUT)
    FILTER_PIXELATE = 2,  // 按键8 - 像素风 (LUT 调色板映射 + 空间像素化)
    FILTER_CUSTOM   = 3   // 按键9 - 自定义 LUT 选单 (预编译 LUT)
};

extern FilterMode currentFilter;

class FilterManager {
public:
    // 获取/设置当前滤镜
    static void setFilter(FilterMode mode);
    static FilterMode getFilter();

    // 循环切换自定义 LUT 滤镜
    static void cycleCustomLut();

    // 获取当前自定义 LUT 滤镜名称
    static const char* getCustomLutName();

    // 滤镜菜单列表状态与操作接口
    static bool isFilterListOpen();
    static void toggleFilterList();
    static int getMenuSelectedIndex();
    static void setMenuSelectedIndex(int index);
    static void cycleMenuSelection(int direction);
    static int getMenuCount();
    static const char* getMenuIndexName(int idx);

    /**
     * 对 canvas 局部区域施加当前滤镜
     * @param canvas  目标画布
     * @param x,y     绘制区域左上角
     * @param w,h     有效图像宽高
     */
    static void applyToCanvas(M5Canvas& canvas, int x, int y, int w, int h);

private:
    // 通用 LUT 颜色映射 (所有纯色滤镜共用)
    static void applyLUT(M5Canvas& canvas, int x, int y, int w, int h,
                         const uint16_t* lut);

    // 像素化空间效果 (LUT 映射 + block fill)
    static void applyPixelate(M5Canvas& canvas, int x, int y, int w, int h,
                              const uint16_t* lut);

    // 获取当前滤镜对应的 LUT 指针
    static const uint16_t* getCurrentLUT();
};

#endif // FILTER_MANAGER_H
