#ifndef FILTER_MANAGER_H
#define FILTER_MANAGER_H

#include <Arduino.h>
#include <M5Cardputer.h>
#include "Global.h"

// 滤镜模式枚举
enum FilterMode {
    FILTER_NONE     = 0,
    FILTER_GAMEBOY  = 1,  // 按键7
    FILTER_PIXELATE = 2,  // 按键8
    FILTER_GLITCH   = 3   // 按键9
};

extern FilterMode currentFilter;

class FilterManager {
public:
    // 获取/设置当前滤镜
    static void setFilter(FilterMode mode);
    static FilterMode getFilter();

    /**
     * 对 canvas 局部区域施加当前滤镜
     * @param canvas  目标画布
     * @param x,y     绘制区域左上角（居中裁切后的起点）
     * @param w,h     有效图像宽高
     */
    static void applyToCanvas(M5Canvas& canvas, int x, int y, int w, int h);

private:
    // GameBoy 绿色4阶风格
    static void applyGameboy(M5Canvas& canvas, int x, int y, int w, int h);
    // 像素块化
    static void applyPixelate(M5Canvas& canvas, int x, int y, int w, int h);
    // 故障错位
    static void applyGlitch(M5Canvas& canvas, int x, int y, int w, int h);

    // 内部辅助：将 RGB565 拆分为 R5G6B5 分量（整数）
    static inline void rgb565Split(uint16_t c, int& r, int& g, int& b) {
        r = (c >> 11) & 0x1F;   // 5bit
        g = (c >> 5)  & 0x3F;   // 6bit
        b =  c        & 0x1F;   // 5bit
    }
    // 将 5/6/5bit 分量合并回 RGB565
    static inline uint16_t rgb565Merge(int r, int g, int b) {
        return ((uint16_t)(r & 0x1F) << 11) |
               ((uint16_t)(g & 0x3F) << 5)  |
               ((uint16_t)(b & 0x1F));
    }
    // 从 5bit R 和 B，6bit G 计算灰度（整数近似 ×2 避免溢出）
    // gray8 = (30*r8 + 59*g8 + 11*b8) / 100
    // r8 = r5<<3, g8 = g6<<2, b8 = b5<<3
    static inline int rgb565Gray(int r5, int g6, int b5) {
        // 扩展到8bit近似
        int r8 = r5 << 3;
        int g8 = g6 << 2;
        int b8 = b5 << 3;
        return (30 * r8 + 59 * g8 + 11 * b8) / 100;
    }
};

#endif // FILTER_MANAGER_H
