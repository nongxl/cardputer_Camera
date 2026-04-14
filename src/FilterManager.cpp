/**
 * FilterManager.cpp
 * 
 * 实时摄像头滤镜处理器
 * 
 * 注意：M5Canvas 底层是 LovyanGFX，读写像素使用 readPixelValue / drawPixel。
 * RGB565 格式（16bit）：RRRRRGGGGGGBBBBB
 * 
 * 滤镜均作用于 canvas 已绘制区域，以整数运算实现，无浮点。
 */

#include "FilterManager.h"

// ── 滤镜效果强度配置 (可自由修改以下参数) ─────────────────────────
// 【像素风与动漫化】
#define CONFIG_PIXELATE_BLOCK_SIZE    4     // 马赛克色块大小 (平衡辨识度与硬核像素感)
// ──────────────────────────────────────────────────────────────

// Pico-8 专用 16 色经典调色板 (RGB565 预计算)
static const uint16_t PICO8_PALETTE[] = {
    0x0000, 0x194A, 0x792A, 0x042A, 0xAD66, 0x5AD9, 0xC618, 0xFF9D,
    0xF809, 0xFD00, 0xFFE4, 0x0726, 0x2D7F, 0x83B3, 0xFBB5, 0xFE75
};

// 工具：在调色板中寻找视觉最接近的颜色 (欧几里得距离平方)
static uint16_t findClosestPaletteColor(uint16_t c) {
    int r1 = (c >> 11) & 0x1F;
    int g1 = (c >> 5)  & 0x3F;
    int b1 = c         & 0x1F;

    uint16_t bestColor = PICO8_PALETTE[0];
    long minDistance = 2000000; // 足够大的初始值

    for (int i = 0; i < 16; i++) {
        uint16_t p = PICO8_PALETTE[i];
        int dr = r1 - ((p >> 11) & 0x1F);
        int dg = g1 - ((p >> 5)  & 0x3F);
        int db = b1 - (p         & 0x1F);
        
        // 简单加权: G通道对亮度贡献大，乘以2提高匹配准确度
        long dist = (long)dr*dr + (long)dg*dg*2 + (long)db*db;
        
        if (dist < minDistance) {
            minDistance = dist;
            bestColor = p;
        }
    }
    return bestColor;
}

// 【故障风】
#define CONFIG_GLITCH_ROW_ODDS        242   // 画面撕裂概率: 0~255 (原204=>20%行偏移, 230=>10%, 242=>5%)
#define CONFIG_GLITCH_ROW_SHIFT_MASK  0x7   // 画面撕裂幅度: 0xF=>最大偏移16像素, 0x7=>8像素, 0x3=>4像素
#define CONFIG_GLITCH_RED_SHIFT_MOD   4u    // 红色通道错位幅度: 8u=>最大错位10像素, 4u=>最大错位6像素
// ──────────────────────────────────────────────────────────────

// ── 全局滤镜状态 ──────────────────────────────────────────────
FilterMode currentFilter = FILTER_NONE;

void FilterManager::setFilter(FilterMode mode) {
    currentFilter = mode;
    serialPrintf("[Filter] Mode -> %d\n", (int)mode);
}

FilterMode FilterManager::getFilter() {
    return currentFilter;
}

// ── 对外接口 ─────────────────────────────────────────────────
void FilterManager::applyToCanvas(M5Canvas& canvas, int x, int y, int w, int h) {
    switch (currentFilter) {
        case FILTER_GAMEBOY:  applyGameboy(canvas, x, y, w, h);  break;
        case FILTER_PIXELATE: applyPixelate(canvas, x, y, w, h); break;
        case FILTER_GLITCH:   applyGlitch(canvas, x, y, w, h);   break;
        default: break;
    }
}

// ── 内部工具：色彩鲜艳度增强 ─────────────────────────────────
// 用于补偿修复端序后原相机自然颜色偏淡的问题
inline uint16_t boostVibrance(uint16_t c) {
    int r = (c >> 11) & 0x1F;
    int g = (c >> 5)  & 0x3F;
    int b = c         & 0x1F;

    int r8 = (r * 255) / 31;
    int g8 = (g * 255) / 63;
    int b8 = (b * 255) / 31;

    // 计算快速明度灰化值 Luma
    int luma = (r8 * 3 + g8 * 6 + b8 * 1) / 10;

    // 饱和度拉升系数 2.2x 
    r8 = luma + (r8 - luma) * 22 / 10;
    g8 = luma + (g8 - luma) * 22 / 10;
    b8 = luma + (b8 - luma) * 22 / 10;

    // 限幅
    if (r8 > 255) r8 = 255; else if (r8 < 0) r8 = 0;
    if (g8 > 255) g8 = 255; else if (g8 < 0) g8 = 0;
    if (b8 > 255) b8 = 255; else if (b8 < 0) b8 = 0;

    return ((r8 * 31 / 255) << 11) | ((g8 * 63 / 255) << 5) | (b8 * 31 / 255);
}

// ═══════════════════════════════════════════════════════════════
// 滤镜1：GameBoy 风
// 算法：
//   1. 逐像素读取 RGB565 → 计算灰度（整数 ITU-R BT.601 近似）
//   2. 量化为 4 级灰度（0/85/170/255）
//   3. 映射到 GameBoy 经典绿色调色板
//      Level0→深绿 #0f380f  Level1→中绿 #306230
//      Level2→浅绿 #8bac0f  Level3→亮绿 #9bbc0f
// ═══════════════════════════════════════════════════════════════
void FilterManager::applyGameboy(M5Canvas& canvas, int x, int y, int w, int h) {
    // GameBoy 调色板（4色，RGB888→RGB565 预计算）
    // #0f380f → R=1  G=13 B=1   → R5=0  G6=6  B5=0  → 0x0180
    // #306230 → R=48 G=98 B=48  → R5=6  G6=24 B5=6  → 0x3306
    // #8bac0f → R=139G=172B=15  → R5=17 G6=43 B5=1  → 0x8AE1
    // #9bbc0f → R=155G=188B=15  → R5=19 G6=47 B5=1  → 0x9BE1
    static const uint16_t gbPalette[4] = {
        ((uint16_t)0  << 11) | ((uint16_t)6  << 5) | 0,   // 最暗
        ((uint16_t)6  << 11) | ((uint16_t)24 << 5) | 6,   // 次暗
        ((uint16_t)17 << 11) | ((uint16_t)43 << 5) | 1,   // 次亮
        ((uint16_t)19 << 11) | ((uint16_t)47 << 5) | 1    // 最亮
    };

    int endX = x + w;
    int endY = y + h;

    for (int py = y; py < endY; py++) {
        for (int px = x; px < endX; px++) {
            uint16_t c = canvas.readPixel(px, py);
            int r, g, b;
            rgb565Split(c, r, g, b);

            int gray = rgb565Gray(r, g, b); // 0~255

            // 量化到4级 [0,85,170,255]
            int level;
            if      (gray < 64)  level = 0;
            else if (gray < 128) level = 1;
            else if (gray < 192) level = 2;
            else                 level = 3;

            canvas.drawPixel(px, py, gbPalette[level]);
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// 滤镜2：像素风（Pixelate）
// 算法：
//   将图像分成 BLOCK×BLOCK 的块
//   取块内 (左上角) 像素颜色填充整个块
//   使用左上角策略（零额外内存，最快速）
//   块大小 8×8（串流分辨率 320×240 → 40×30 块，低像素风强烈）
// ═══════════════════════════════════════════════════════════════
void FilterManager::applyPixelate(M5Canvas& canvas, int x, int y, int w, int h) {
    const int BLOCK = CONFIG_PIXELATE_BLOCK_SIZE;

    int endX = x + w;
    int endY = y + h;

    for (int by = y; by < endY; by += BLOCK) {
        for (int bx = x; bx < endX; bx += BLOCK) {
            // 采样当前块左上角像素，应用鲜艳度增强
            uint16_t c = boostVibrance(canvas.readPixel(bx, by));
            
            // 核心：将其映射到最接近的 PICO8 复古调色板颜色
            // 调色板映射能有效消除相机噪点产生的“非风格化”杂色
            uint16_t blockColor = findClosestPaletteColor(c);

            // 填充整个块
            int bEndX = bx + BLOCK; if (bEndX > endX) bEndX = endX;
            int bEndY = by + BLOCK; if (bEndY > endY) bEndY = endY;
            canvas.fillRect(bx, by, bEndX - bx, bEndY - by, blockColor);
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// 滤镜3：故障风（Glitch）
// 算法（两种混合，每帧随机选其中一种行执行）：
//   A）水平行偏移：随机选15%行向左或向右平移1~12像素（环绕）  
//   B）颜色通道分离：每帧随机对红色通道施加 +X 列横向偏移
// 使用 LCG 快速伪随机，避免 rand()
// ═══════════════════════════════════════════════════════════════
void FilterManager::applyGlitch(M5Canvas& canvas, int x, int y, int w, int h) {
    static uint32_t seed = 42;
    auto lcg = [&]() -> uint32_t {
        seed = seed * 1664525u + 1013904223u;
        return seed;
    };

    int endX = x + w;
    int endY = y + h;
    
    // 修正：将缓冲区扩大至 320 像素，确保支持 240 高清后期处理而不发生溢出
    static uint16_t rowBuf[320]; 
    if (w > 320) w = 320; // 边界安全保护
    
    // 随机全屏闪烁概率（反色或白闪）
    uint32_t globalRnd = lcg();
    bool doInvert = (globalRnd % 100 == 0); 

    for (int py = y; py < endY; py++) {
        uint32_t rnd = lcg();
        
        // 1. 基础读取与色彩强化
        for (int i = 0; i < w; i++) {
            rowBuf[i] = boostVibrance(canvas.readPixel(x + i, py));
            if (doInvert) rowBuf[i] = ~rowBuf[i];
        }

        // 2. 确定当前行的“病态”程度
        bool hasJitter = (rnd % 100 < 15);     // 15% 概率产生行位移
        bool hasColorSplit = (rnd % 100 < 35); // 35% 概率产生色散
        bool isScanline = (py % 2 == 0);       // CRT 隔行扫描暗线

        int shift = hasJitter ? (int)((rnd >> 8) % 6u) - 3 : 0; // -3 ~ +3 像素微抖动
        int split = hasColorSplit ? (int)((rnd >> 12) % 4u) + 1 : 0; // 1 ~ 4 像素色散

        // 3. 像素合成处理回写
        for (int i = 0; i < w; i++) {
            int srcIdx = (i + shift + w) % w;
            uint16_t finalColor;

            if (hasColorSplit) {
                // 色散合成：取左侧像素的 R，当前像素的 G，右侧像素的 B
                int rIdx = (srcIdx + split) % w;
                int bIdx = (srcIdx - split + w) % w;
                
                uint16_t cR = rowBuf[rIdx];
                uint16_t cG = rowBuf[srcIdx];
                uint16_t cB = rowBuf[bIdx];

                // 提取分量并组合 (RGB565: R5 G6 B5)
                finalColor = (cR & 0xF800) | (cG & 0x07E0) | (cB & 0x001F);
            } else {
                finalColor = rowBuf[srcIdx];
            }

            // 4. 应用扫描线暗化效果 (亮度降低 25%)
            if (isScanline) {
                finalColor = ((finalColor & 0xF800) >> 1 & 0xF800) | 
                             ((finalColor & 0x07E0) >> 1 & 0x07E0) | 
                             ((finalColor & 0x001F) >> 1 & 0x001F);
            }

            canvas.drawPixel(x + i, py, finalColor);
        }

        // 5. 随机大块水平撕裂（覆盖层）
        if (rnd % 500 == 0) {
            int tearLen = (int)((lcg() >> 4) % 20u) + 5;
            int tearOffset = (int)((lcg() >> 8) % 15u) - 7;
            for(int i=0; i<w; i++) {
                int si = (i + tearOffset + w) % w;
                canvas.drawPixel(x + i, py, rowBuf[si]);
            }
            // 撕裂行多处理几次增加厚度
            if (py + 1 < endY) py++; 
        }
    }
}
