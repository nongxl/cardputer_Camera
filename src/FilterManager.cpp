/**
 * FilterManager.cpp
 *
 * LUT (Look-Up Table) 架构的实时摄像头滤镜处理器
 *
 * 核心思路：将所有颜色变换预计算为 65536 条目的 RGB565 查表，
 * 运行时每像素仅需一次内存访问，无任何算术运算。
 * LUT 数据以 static const 形式存放在 Flash (.rodata)，不占用 SRAM。
 *
 * 滤镜列表：
 *   1. GameBoy  - 纯 LUT：灰度量化 → 4色绿调色板
 *   2. Pixelate - 混合：LUT 调色板映射 + 空间像素化 (block fill)
 *   3. Film     - 纯 LUT：复古胶片暖色调
 */

#include "FilterManager.h"
#include "lut_gameboy.h"
#include "lut_pixelate.h"
#include "lut_custom_registry.h"

// ── 配置 ─────────────────────────────────────────────────────
#define CONFIG_PIXELATE_BLOCK_SIZE  4   // 像素化马赛克块大小

// ── 全局滤镜状态 ──────────────────────────────────────────────
FilterMode currentFilter = FILTER_NONE;
static int currentCustomLutIndex = -1;  // 当前激活的自定义 LUT 索引 (-1 表示未激活)
static bool filterListOpen = false;     // 滤镜选择列表是否打开
static int menuSelectedIndex = 0;       // 当前在菜单中选中的索引

void FilterManager::setFilter(FilterMode mode) {
    if (mode != FILTER_CUSTOM) {
        currentCustomLutIndex = -1;
    }
    currentFilter = mode;
    serialPrintf("[Filter] Mode -> %d (CustomIdx -> %d)\n", (int)mode, currentCustomLutIndex);
}

FilterMode FilterManager::getFilter() {
    return currentFilter;
}

// 滤镜菜单开关状态
bool FilterManager::isFilterListOpen() {
    return filterListOpen;
}

void FilterManager::toggleFilterList() {
    filterListOpen = !filterListOpen;
    if (filterListOpen) {
        // 开启菜单时，自动将高亮行定位到当前实际生效的自定义滤镜或 None 上
        if (currentFilter == FILTER_CUSTOM) {
            menuSelectedIndex = 1 + currentCustomLutIndex;
        } else {
            menuSelectedIndex = 0;
        }
        serialPrintf("[Filter] Menu Opened, focused on index %d\n", menuSelectedIndex);
    } else {
        serialPrintf("[Filter] Menu Closed\n");
    }
}

int FilterManager::getMenuSelectedIndex() {
    return menuSelectedIndex;
}

void FilterManager::setMenuSelectedIndex(int index) {
    int total = getMenuCount();
    if (index >= 0 && index < total) {
        menuSelectedIndex = index;
        // 选中即直接套用
        if (menuSelectedIndex == 0) {
            setFilter(FILTER_NONE);
        } else {
            currentCustomLutIndex = menuSelectedIndex - 1;
            setFilter(FILTER_CUSTOM);
        }
        serialPrintf("[Filter] Menu select -> %d (%s)\n", menuSelectedIndex, getMenuIndexName(menuSelectedIndex));
    }
}

void FilterManager::cycleMenuSelection(int direction) {
    int total = getMenuCount();
    if (total == 0) return;
    int next = menuSelectedIndex + direction;
    if (next < 0) next = total - 1;
    if (next >= total) next = 0;
    setMenuSelectedIndex(next);
}

int FilterManager::getMenuCount() {
    return 1 + CUSTOM_LUT_COUNT; // 仅含 None (1) + 自定义 LUT 文件数量
}

const char* FilterManager::getMenuIndexName(int idx) {
    if (idx == 0) return "None (Clear)";
    if (idx >= 1 && idx < 1 + CUSTOM_LUT_COUNT) {
        return CUSTOM_LUTS[idx - 1].name;
    }
    return "";
}

// 获取当前自定义 LUT 滤镜名称
const char* FilterManager::getCustomLutName() {
    if (currentFilter == FILTER_CUSTOM && currentCustomLutIndex >= 0 && currentCustomLutIndex < CUSTOM_LUT_COUNT) {
        return CUSTOM_LUTS[currentCustomLutIndex].name;
    }
    return "";
}

// ── LUT 指针获取 ──────────────────────────────────────────────
const uint16_t* FilterManager::getCurrentLUT() {
    switch (currentFilter) {
        case FILTER_GAMEBOY:  return LUT_GAMEBOY;
        case FILTER_PIXELATE: return LUT_PIXELATE;
        case FILTER_CUSTOM:
            if (currentCustomLutIndex >= 0 && currentCustomLutIndex < CUSTOM_LUT_COUNT) {
                return CUSTOM_LUTS[currentCustomLutIndex].lut;
            }
            return nullptr;
        default:              return nullptr;
    }
}

// ── 通用 LUT 颜色映射 (纯色滤镜共用) ──────────────────────────
void FilterManager::applyLUT(M5Canvas& canvas, int x, int y, int w, int h,
                              const uint16_t* lut) {
    int endX = x + w;
    int endY = y + h;
    for (int py = y; py < endY; py++) {
        for (int px = x; px < endX; px++) {
            uint16_t c = canvas.readPixel(px, py);
            uint16_t idx = ((c >> 1) & 0x7FE0) | (c & 0x001F);
            canvas.drawPixel(px, py, lut[idx]);
        }
    }
}

// ── Pixelate Posterize: Photoshop 式色阶硬阶梯坍缩 + 纯色块靠拢像素风 ───────
void FilterManager::applyPixelate(M5Canvas& canvas, int x, int y, int w, int h,
                                   const uint16_t* lut) {
    int endX = x + w;
    int endY = y + h;
    const int BLOCK = 3; // 3x3 块，兼顾大平涂纯色块感与轮廓可读性

    for (int by = y; by < endY; by += BLOCK) {
        for (int bx = x; bx < endX; bx += BLOCK) {
            int bEndX = bx + BLOCK; if (bEndX > endX) bEndX = endX;
            int bEndY = by + BLOCK; if (bEndY > endY) bEndY = endY;

            // 1. 采样块内平均 RGB
            uint32_t rSum = 0, gSum = 0, bSum = 0;
            int count = 0;
            for (int py = by; py < bEndY; py++) {
                for (int px = bx; px < bEndX; px++) {
                    uint16_t pixel = canvas.readPixel(px, py);
                    rSum += ((pixel >> 11) & 0x1F) << 3;
                    gSum += ((pixel >> 5)  & 0x3F) << 2;
                    bSum += (pixel         & 0x1F) << 3;
                    count++;
                }
            }
            if (count == 0) continue;

            uint8_t avgR = rSum / count;
            uint8_t avgG = gSum / count;
            uint8_t avgB = bSum / count;

            // 2. Photoshop 式 Posterize 阶梯化 (将 0~255 强制塌陷为 4 个硬颜色阶梯)
            auto posterizeChannel = [](uint8_t val) -> uint8_t {
                if (val < 42)  return 0;
                if (val < 128) return 85;
                if (val < 212) return 170;
                return 255;
            };

            uint8_t pR = posterizeChannel(avgR);
            uint8_t pG = posterizeChannel(avgG);
            uint8_t pB = posterizeChannel(avgB);

            // 3. Threshold 边缘极性靠拢强化 (将过渡带向高亮/深暗纯色硬靠拢)
            uint8_t lum = (uint8_t)((pR * 77 + pG * 150 + pB * 29) >> 8);
            if (lum < 30) {
                pR = (pR > 30) ? pR - 30 : 0;
                pG = (pG > 30) ? pG - 30 : 0;
                pB = (pB > 30) ? pB - 30 : 0;
            } else if (lum > 220) {
                pR = 255; pG = 255; pB = 255;
            }

            uint16_t finalColor = ((pR >> 3) << 11) | ((pG >> 2) << 5) | (pB >> 3);

            if (lut) {
                uint16_t idx = ((finalColor >> 1) & 0x7FE0) | (finalColor & 0x001F);
                finalColor = lut[idx];
            }

            // 4. 填充纯色块
            canvas.fillRect(bx, by, bEndX - bx, bEndY - by, finalColor);
        }
    }
}

// ── 对外接口 (签名与旧版完全兼容) ────────────────────────────
void FilterManager::applyToCanvas(M5Canvas& canvas, int x, int y, int w, int h) {
    const uint16_t* lut = getCurrentLUT();
    if (!lut) return;

    if (currentFilter == FILTER_PIXELATE) {
        applyPixelate(canvas, x, y, w, h, lut);
    } else {
        applyLUT(canvas, x, y, w, h, lut);
    }
}
