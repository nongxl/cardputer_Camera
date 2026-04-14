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
    // 极简 LCG 随机数
    static uint32_t seed = 42;
    auto lcg = [&]() -> uint32_t {
        seed = seed * 1664525u + 1013904223u;
        return seed;
    };

    int endX = x + w;
    int endY = y + h;

    // ── A：水平行随机偏移 ────────────────────────────────────
    // 对约 20% 的行做水平平移
    // 使用临时行缓冲（静态，节省堆空间）
    static uint16_t rowBuf[240]; // 行缓存（最大拍摄宽度 240，实际截图宽度）
    // 实际宽度
    int line_w = (w < 240) ? w : 240;

    for (int py = y; py < endY; py++) {
        uint32_t rnd = lcg();
        // 概率触发本行偏移
        if ((rnd & 0xFF) > CONFIG_GLITCH_ROW_ODDS) continue;

        int shift = (int)(rnd >> 8 & CONFIG_GLITCH_ROW_SHIFT_MASK) + 1; // 偏移量

        int dir   = (rnd >> 4) & 1;             // 0=左 1=右

        // 读取整行到缓冲，并同时执行色彩强化
        for (int px = x; px < x + line_w; px++) {
            rowBuf[px - x] = boostVibrance(canvas.readPixel(px, py));
        }

        // 写回（带偏移，环绕）
        for (int i = 0; i < line_w; i++) {
            int srcIdx;
            if (dir == 0) {
                srcIdx = (i + shift) % line_w;          // 左移
            } else {
                srcIdx = (i - shift + line_w) % line_w; // 右移
            }
            canvas.drawPixel(x + i, py, rowBuf[srcIdx]);
        }
    }

    // ── B：红色通道横向错位 ──────────────────────────────────
    // 选一条随机水平带（高度约1/6～1/4），对红色通道做+N列偏移
    {
        uint32_t rnd2 = lcg();
        int bandStart = y + (int)(rnd2 % (uint32_t)h);
        int bandH     = 1 + (int)((lcg() >> 2) % 8u);   // 1~8 行高
        if (bandStart + bandH > endY) bandH = endY - bandStart;
        int redShift  = 2 + (int)((lcg() >> 3) % CONFIG_GLITCH_RED_SHIFT_MOD);   // 红色错位幅度

        for (int py = bandStart; py < bandStart + bandH; py++) {
            for (int px = x; px < endX; px++) {
                // 读取并强化当前像素与移位像素的色彩
                uint16_t c = boostVibrance(canvas.readPixel(px, py));
                int srcX = px + redShift;
                if (srcX >= endX) srcX = endX - 1;
                uint16_t cR = boostVibrance(canvas.readPixel(srcX, py));

                int r0, g0, b0, rR, gR, bR;
                rgb565Split(c,  r0, g0, b0);
                rgb565Split(cR, rR, gR, bR);

                // 混合：用 cR 的红色通道替换原像素
                canvas.drawPixel(px, py, rgb565Merge(rR, g0, b0));
            }
        }
    }
}
