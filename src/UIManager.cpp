#include "UIManager.h"
#include <ArduinoJson.h>
#include <SD.h>
#include <vector>
#include "StorageManager.h"
#include "MjpegParser.h"
#include "FilterManager.h"
#include "InputHandler.h"

// 外部引用或定义 Canvas 指针（安全延迟构造，防静态全局解引用未初始化的 Display 引发开机 RTC_SW_SYS_RST 崩溃）
M5Canvas* canvas = nullptr;
M5Canvas* mainCanvas = nullptr;

static uint32_t bootStartTime = 0;

void UIManager::init() {
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.fillScreen(M5_COLOR_DARK_BG);
    
    if (canvas == nullptr) canvas = new M5Canvas(&M5Cardputer.Display);
    if (mainCanvas == nullptr) mainCanvas = new M5Canvas(&M5Cardputer.Display);
    
    canvas->setColorDepth(16);
    canvas->createSprite(160, 120);
    
    mainCanvas->setColorDepth(16);
    mainCanvas->createSprite(240, 135);
    
    bootStartTime = millis();
    displayLine("Initializing system...", true);
}

void UIManager::displayLine(String line, bool reset) {
    static std::vector<String> logHistory;
    if (reset) {
        logHistory.clear();
    }
    
    logHistory.push_back(line);
    if (logHistory.size() > 5) {
        logHistory.erase(logHistory.begin());
    }
    
    mainCanvas->fillSprite(M5_COLOR_DARK_BG);
    
    // 1. 上半部分：炫彩手机级开机 Banner & 流光动画
    uint16_t rainbowColors[] = { M5_COLOR_ORANGE, M5_COLOR_YELLOW, M5_COLOR_CYAN, M5_COLOR_PINK, M5_COLOR_GREEN };
    int numColors = 5;
    uint32_t elapsed = millis() - bootStartTime;
    int step = (elapsed / 50) % 20;
    
    // 动态脉冲镜头光圈 (Pulse Rings)
    int centerX = 30;
    int centerY = 27;
    for (int r = 2; r >= 1; r--) {
        int radius = (int)(r * 8 + (step % 5));
        uint16_t color = rainbowColors[(step + r) % numColors];
        mainCanvas->drawCircle(centerX, centerY, radius, color);
    }
    
    // 贴纸 Logo Banner (M5 CARDPUTER CAM)
    int bannerX = 54;
    int bannerY = 16;
    int bannerW = 150;
    int bannerH = 22;
    mainCanvas->fillRect(bannerX, bannerY, bannerW, bannerH, M5_COLOR_ORANGE);
    mainCanvas->drawRect(bannerX - 1, bannerY - 1, bannerW + 2, bannerH + 2, M5_COLOR_YELLOW);
    mainCanvas->setTextColor(M5_COLOR_BLACK);
    mainCanvas->setTextSize(1);
    mainCanvas->setCursor(bannerX + 12, bannerY + 7);
    mainCanvas->print("M5 CARDPUTER CAM");
    
    // 右上角旋转 Spinner 字符指示器
    const char* spinChars[] = { "|", "/", "-", "\\" };
    mainCanvas->setTextColor(M5_COLOR_CYAN);
    mainCanvas->setCursor(215, 23);
    mainCanvas->print(spinChars[step % 4]);

    // 分割贴纸 Line
    mainCanvas->fillRect(10, 52, 220, 2, M5_COLOR_CYAN);

    // 2. 下半部分：实地并发开机日志 Terminal
    for (size_t i = 0; i < logHistory.size(); i++) {
        int yPos = 58 + i * 13;
        if (i == logHistory.size() - 1) {
            // 当前最新正在执行的阶段（亮黄底卡片高亮）
            mainCanvas->fillRect(10, yPos - 1, 220, 12, M5_COLOR_CARD_BG);
            mainCanvas->drawRect(10, yPos - 1, 220, 12, M5_COLOR_YELLOW);
            mainCanvas->setTextColor(M5_COLOR_YELLOW);
            mainCanvas->setCursor(14, yPos + 1);
            mainCanvas->printf("> %s", logHistory[i].c_str());
        } else {
            // 历史步骤
            mainCanvas->setTextColor(M5_COLOR_WHITE);
            mainCanvas->setCursor(18, yPos + 1);
            mainCanvas->printf("  %s", logHistory[i].c_str());
        }
    }

    // 3. 底部绚彩 5 色彩虹跑马边框
    for (int c = 0; c < 5; c++) {
        mainCanvas->fillRect(c * 48, 132, 48, 3, rainbowColors[(c + step) % numColors]);
    }
    
    // 推送至屏幕，彻底告别黑屏
    mainCanvas->pushSprite(&M5Cardputer.Display, 0, 0);
}

void UIManager::drawCaptureOverlay() {
    if (appState.overlayTimestamp == 0) return;
    
    if (millis() - appState.overlayTimestamp < 5000) {
        int textW = strlen(appState.overlayMsg) * 6;
        int boxW = textW + 12;
        int boxH = 15;
        int boxX = 6;
        int boxY = 116;
        
        // M5 贴纸胶囊底框：粉紫背景 + 鲜黄双边框
        mainCanvas->fillRect(boxX, boxY, boxW, boxH, M5_COLOR_PINK);
        mainCanvas->drawRect(boxX, boxY, boxW, boxH, M5_COLOR_YELLOW);
        mainCanvas->setTextColor(M5_COLOR_WHITE);
        mainCanvas->setTextSize(1);
        mainCanvas->setCursor(boxX + 6, boxY + 4);
        mainCanvas->print(appState.overlayMsg);
    } else {
        appState.overlayTimestamp = 0;
    }
}

void UIManager::showStatus(const String& statusJson) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, statusJson);
    
    struct KeyVal { String key; String val; };
    std::vector<KeyVal> items;
    
    if (error) {
        items.push_back({"Error", "JSON Parse Fail"});
    } else {
        auto add = [&](const char* label, const char* jsonKey) {
            items.push_back({String(label), doc[jsonKey].as<String>()});
        };
        add("Resolution", "framesize");
        add("Quality", "quality");
        add("Brightness", "brightness");
        add("Contrast", "contrast");
        add("Saturation", "saturation");
        add("Sharpness", "sharpness");
        add("Special FX", "special_effect");
        add("WB Mode", "wb_mode");
        add("AEC Value", "aec_value");
        add("AGC Gain", "agc_gain");
    }

    int offset = 0;
    const int visible = 8;

    while (true) {
        mainCanvas->fillSprite(M5_COLOR_DARK_BG);
        
        // Header Banner (亮橙底 + 黑色粗体)
        mainCanvas->fillRect(0, 0, 240, 20, M5_COLOR_ORANGE);
        mainCanvas->setTextColor(M5_COLOR_BLACK);
        mainCanvas->setTextSize(1);
        mainCanvas->setCursor(10, 6);
        mainCanvas->print("--- CAMERA STATUS ---");
        
        // 绘制列表项
        for (int i = 0; i < visible && (offset + i) < (int)items.size(); i++) {
            int yPos = 25 + i * 11;
            
            if (i % 2 == 0) {
                mainCanvas->fillRect(6, yPos - 1, 222, 10, M5_COLOR_CARD_BG);
            }
            
            // Key 使用 Cyber Teal
            mainCanvas->setTextColor(M5_COLOR_CYAN);
            mainCanvas->setCursor(10, yPos);
            mainCanvas->printf("%-12s:", items[offset + i].key.c_str());
            
            // Value 使用 Vibrant Yellow
            mainCanvas->setTextColor(M5_COLOR_YELLOW);
            mainCanvas->setCursor(100, yPos);
            mainCanvas->print(items[offset + i].val);
        }
        
        // 滚动条 (亮橙滑块 + 电光青轨道)
        if (items.size() > (size_t)visible) {
            int bh = 85, th = bh * visible / items.size();
            int ty = 25 + (bh - th) * offset / (items.size() - visible);
            mainCanvas->fillRect(232, 25, 3, bh, M5_COLOR_CARD_BG);
            mainCanvas->drawRect(232, 25, 3, bh, M5_COLOR_CYAN);
            mainCanvas->fillRect(232, ty, 3, th, M5_COLOR_ORANGE);
        }

        // Footer Banner (鲜黄胶囊提示)
        mainCanvas->fillRect(10, 118, 220, 14, M5_COLOR_YELLOW);
        mainCanvas->setTextColor(M5_COLOR_BLACK);
        mainCanvas->setCursor(45, 121);
        mainCanvas->print("ESC / Enter: Return");

        mainCanvas->pushSprite(&M5Cardputer.Display, 0, 0);
        M5Cardputer.update();
        InputEvent ev = InputHandler::handle();
        
        if (ev == EVENT_BRIGHTNESS_UP) {
            if (offset > 0) offset--;
        } else if (ev == EVENT_BRIGHTNESS_DOWN) {
            if (offset < (int)items.size() - visible) offset++;
        } else if (ev == EVENT_STATUS || ev == EVENT_RESTART) {
            break;
        } else if (ev != EVENT_NONE && ev != EVENT_BRIGHTNESS_UP && ev != EVENT_BRIGHTNESS_DOWN) {
            break;
        }
        delay(10);
    }
}

void UIManager::showWiFiPortal() {
    mainCanvas->fillSprite(M5_COLOR_DARK_BG);
    
    // Header Banner (鲜黄底黑字)
    mainCanvas->fillRect(0, 0, 240, 22, M5_COLOR_YELLOW);
    mainCanvas->setTextColor(M5_COLOR_BLACK);
    mainCanvas->setTextSize(2);
    mainCanvas->setCursor(15, 3);
    mainCanvas->print("WIFI FILE SERVER");

    mainCanvas->setTextSize(1);
    
    // 步骤 1
    mainCanvas->fillRect(10, 32, 20, 14, M5_COLOR_CYAN);
    mainCanvas->setTextColor(M5_COLOR_BLACK);
    mainCanvas->setCursor(17, 35);
    mainCanvas->print("1");
    mainCanvas->setTextColor(M5_COLOR_WHITE);
    mainCanvas->setCursor(36, 35);
    mainCanvas->print("Connect: Cardputer-Cam");

    // 步骤 2
    mainCanvas->fillRect(10, 52, 20, 14, M5_COLOR_CYAN);
    mainCanvas->setTextColor(M5_COLOR_BLACK);
    mainCanvas->setCursor(17, 55);
    mainCanvas->print("2");
    mainCanvas->setTextColor(M5_COLOR_WHITE);
    mainCanvas->setCursor(36, 55);
    mainCanvas->print("No Password Required");

    // 步骤 3
    mainCanvas->fillRect(10, 72, 20, 14, M5_COLOR_CYAN);
    mainCanvas->setTextColor(M5_COLOR_BLACK);
    mainCanvas->setCursor(17, 75);
    mainCanvas->print("3");
    mainCanvas->setTextColor(M5_COLOR_YELLOW);
    mainCanvas->setCursor(36, 75);
    mainCanvas->print("URL: http://192.168.4.1");
    
    // 底部 Exit Badge Banner (亮橙底黑字)
    mainCanvas->fillRect(10, 108, 220, 18, M5_COLOR_ORANGE);
    mainCanvas->setTextColor(M5_COLOR_BLACK);
    mainCanvas->setCursor(32, 113);
    mainCanvas->print("Press 'W' to Exit & Resume");
    
    mainCanvas->pushSprite(&M5Cardputer.Display, 0, 0);
}

void UIManager::showUsbPortal() {
    mainCanvas->fillSprite(M5_COLOR_DARK_BG);
    
    // Header Banner (电光青底黑字)
    mainCanvas->fillRect(0, 0, 240, 22, M5_COLOR_CYAN);
    mainCanvas->setTextColor(M5_COLOR_BLACK);
    mainCanvas->setTextSize(2);
    mainCanvas->setCursor(15, 3);
    mainCanvas->print("USB MSC STORAGE");

    mainCanvas->setTextSize(1);
    if (!isSDInitialized) {
        mainCanvas->fillRect(10, 35, 220, 18, M5_COLOR_PINK);
        mainCanvas->setTextColor(M5_COLOR_WHITE);
        mainCanvas->setCursor(16, 40);
        mainCanvas->println("SD Card NOT Initialized!");
        
        mainCanvas->setTextColor(M5_COLOR_WHITE);
        mainCanvas->setCursor(10, 60);
        mainCanvas->println("Insert SD card & restart device.");
    } else {
        mainCanvas->setTextColor(M5_COLOR_WHITE);
        mainCanvas->setCursor(10, 36);
        mainCanvas->println("SD Card mounted as USB Drive.");
        mainCanvas->setTextColor(M5_COLOR_YELLOW);
        mainCanvas->setCursor(10, 52);
        mainCanvas->println("Photos path: /images/ folder.");
        mainCanvas->setTextColor(M5_COLOR_CYAN);
        mainCanvas->setCursor(10, 68);
        mainCanvas->println("Eject safely on PC before unplug!");
    }
    
    // 底部 Exit Badge Banner (亮橙底黑字)
    mainCanvas->fillRect(10, 108, 220, 18, M5_COLOR_ORANGE);
    mainCanvas->setTextColor(M5_COLOR_BLACK);
    mainCanvas->setCursor(20, 113);
    mainCanvas->print("Press 'U' or '`' to Exit & Resume");
    
    mainCanvas->pushSprite(&M5Cardputer.Display, 0, 0);
}

void UIManager::showHelp() {
    struct KeyHelp { const char* key; const char* desc; };
    static const KeyHelp items[] = {
        {"BtnA", "Capture / Burst"},
        {"T",    "Timelapse Mode"},
        {"W",    "WiFi File Server"},
        {"U",    "USB Storage Mode"},
        {"R",    "Restart Device"},
        {"`",    "Camera Status"},
        {"H",    "Help Menu"},
        {"7",    "Filter - GameBoy"},
        {"8",    "Filter - Pixelate"},
        {"9",    "Custom LUT Menu"},
        {";/.",  "Brightness +/-"},
        {"[/]",  "Saturation +/-"},
        {"_/=",  "Sharpness +/-"},
        {",//",  "Contrast +/-"},
        {"ESC",  "Return Finder"}
    };
    const int totalItems = sizeof(items) / sizeof(items[0]);

    int offset = 0;
    const int visible = 7;

    while (true) {
        mainCanvas->fillSprite(M5_COLOR_DARK_BG);
        
        // Banner Header (亮橙底黑字)
        mainCanvas->fillRect(0, 0, 240, 20, M5_COLOR_ORANGE);
        mainCanvas->setTextColor(M5_COLOR_BLACK);
        mainCanvas->setTextSize(1);
        mainCanvas->setCursor(20, 6);
        mainCanvas->print("CARDPUTER KEYMAP & HELP");
        
        for (int i = 0; i < visible && (offset + i) < totalItems; i++) {
            int yPos = 24 + i * 13;
            
            // 贴纸键帽（黄底黑字框）
            int kw = strlen(items[offset + i].key) * 6 + 8;
            mainCanvas->fillRect(8, yPos, kw, 11, M5_COLOR_YELLOW);
            mainCanvas->setTextColor(M5_COLOR_BLACK);
            mainCanvas->setCursor(12, yPos + 2);
            mainCanvas->print(items[offset + i].key);
            
            // 对应描述
            mainCanvas->setTextColor(M5_COLOR_WHITE);
            mainCanvas->setCursor(16 + kw, yPos + 2);
            mainCanvas->print(items[offset + i].desc);
        }

        // 滚动条
        if (totalItems > visible) {
            int bh = 90, th = bh * visible / totalItems;
            int ty = 24 + (bh - th) * offset / (totalItems - visible);
            mainCanvas->fillRect(232, 24, 3, bh, M5_COLOR_CARD_BG);
            mainCanvas->drawRect(232, 24, 3, bh, M5_COLOR_CYAN);
            mainCanvas->fillRect(232, ty, 3, th, M5_COLOR_ORANGE);
        }

        // Footer 提示
        mainCanvas->fillRect(10, 118, 220, 14, M5_COLOR_CYAN);
        mainCanvas->setTextColor(M5_COLOR_BLACK);
        mainCanvas->setCursor(45, 121);
        mainCanvas->print("ESC / H: Return Finder");

        mainCanvas->pushSprite(&M5Cardputer.Display, 0, 0);
        M5Cardputer.update();
        InputEvent ev = InputHandler::handle();
        
        if (ev == EVENT_BRIGHTNESS_UP) {
            if (offset > 0) offset--;
        } else if (ev == EVENT_BRIGHTNESS_DOWN) {
            if (offset < totalItems - visible) offset++;
        } else if (ev == EVENT_HELP || ev == EVENT_RESTART) {
            break;
        } else if (ev != EVENT_NONE && ev != EVENT_BRIGHTNESS_UP && ev != EVENT_BRIGHTNESS_DOWN) {
            break;
        }
        delay(10);
    }
}

static void drawFilterMenu() {
    int total = FilterManager::getMenuCount();
    if (total == 0) return;
    
    int selected = FilterManager::getMenuSelectedIndex();
    int visible = 5; // 减少一行高度（由 6 行减少为 5 行）
    int offset = selected - visible / 2;
    if (offset < 0) offset = 0;
    if (offset + visible > total) offset = total - visible;
    if (offset < 0) offset = 0;
    
    // M5 镂空贴纸卡片弹窗（宽度收窄至 160px，与 160x120 取景区域宽度精密对齐）
    int cardX = 40, cardY = 22, cardW = 160, cardH = 100;
    mainCanvas->drawRect(cardX, cardY, cardW, cardH, M5_COLOR_CYAN);
    mainCanvas->drawRect(cardX + 1, cardY + 1, cardW - 2, cardH - 2, M5_COLOR_ORANGE);
    
    // 卡片 Header 贴纸 (亮黄底黑字)
    mainCanvas->fillRect(cardX + 2, cardY + 2, cardW - 4, 16, M5_COLOR_YELLOW);
    mainCanvas->setTextColor(M5_COLOR_BLACK);
    mainCanvas->setTextSize(1);
    mainCanvas->setCursor(cardX + 29, cardY + 6);
    mainCanvas->print("[ FILTER SELECT ]");
    
    // 绘制选项
    for (int i = offset; i < offset + visible && i < total; i++) {
        const char* name = FilterManager::getMenuIndexName(i);
        int yPos = cardY + 24 + (i - offset) * 14;
        
        if (i == selected) {
            // 仅选中项使用亮橙满宽贴纸条 + 黑色粗体
            mainCanvas->fillRect(cardX + 6, yPos - 1, cardW - 12, 13, M5_COLOR_ORANGE);
            mainCanvas->setTextColor(M5_COLOR_BLACK);
            mainCanvas->setCursor(cardX + 10, yPos + 2);
            mainCanvas->printf("> %s", name);
        } else {
            // 未选中项镂空背景，使用鲜黄/电光青高对比度文本透视显示
            mainCanvas->setTextColor(M5_COLOR_CYAN);
            mainCanvas->setCursor(cardX + 10, yPos + 2);
            mainCanvas->print(" ");
            mainCanvas->setTextColor(M5_COLOR_WHITE);
            mainCanvas->print(name);
        }
    }
}

bool UIManager::renderStream() {
    bool drawSuccess = canvas->drawJpg(appState.networkBuffer, appState.networkSize, 0, 0, 0, 0, 0, 0, 0.5f);
    if (drawSuccess && FilterManager::getFilter() != FILTER_NONE) {
        FilterManager::applyToCanvas(*canvas, 0, 0, canvas->width(), canvas->height());
    }
    if (drawSuccess) {
        // 恢复 1.5 倍 Center Crop 填充渲染，使取景图像完美充满 240x135 全屏
        canvas->pushRotateZoom(mainCanvas, mainCanvas->width()/2, mainCanvas->height()/2, 0, 1.5f, 1.5f);
        
        // 1. 左上角 M5-CAM 贴纸 Badge
        mainCanvas->fillRect(4, 4, 48, 14, M5_COLOR_ORANGE);
        mainCanvas->setTextColor(M5_COLOR_BLACK);
        mainCanvas->setTextSize(1);
        mainCanvas->setCursor(8, 7);
        mainCanvas->print("M5-CAM");
        
        // 2. 滤镜 Badge (若启用)
        FilterMode fm = FilterManager::getFilter();
        if (fm != FILTER_NONE) {
            static FilterMode lastFm = FILTER_NONE;
            static char cachedLutName[16] = {0};
            static int cachedLutW = 20;
            
            const char* filterNames[] = { "", "GB", "PIX", "LUT" };
            const char* name = (fm == FILTER_CUSTOM) ? (FilterManager::getCustomLutName()[0] != '\0' ? FilterManager::getCustomLutName() : "LUT") : filterNames[(int)fm];
            
            if (fm != lastFm || strcmp(name, cachedLutName) != 0) {
                lastFm = fm;
                strncpy(cachedLutName, name, sizeof(cachedLutName) - 1);
                cachedLutW = strlen(cachedLutName) * 6 + 12;
            }
            
            mainCanvas->fillRect(54, 4, cachedLutW, 14, M5_COLOR_YELLOW);
            mainCanvas->setTextColor(M5_COLOR_BLACK);
            mainCanvas->setCursor(59, 7);
            mainCanvas->print(cachedLutName);
        }
        
        // 3. 右上角 FPS 与 SRAM 极客监控 Badge (1秒高频缓存，避免逐帧 snprintf/strlen 开销)
        static uint32_t lastOverlayUpdate = 0;
        static char cachedFpsBuf[32] = "0.0fps";
        static int cachedFpsW = 44;
        static int cachedFpsX = 192;
        
        if (millis() - lastOverlayUpdate >= 1000) {
            lastOverlayUpdate = millis();
            uint32_t usedKB = (ESP.getHeapSize() - ESP.getFreeHeap()) / 1024;
            snprintf(cachedFpsBuf, sizeof(cachedFpsBuf), "%.1ffps %uK", currentFps, usedKB);
            cachedFpsW = strlen(cachedFpsBuf) * 6 + 8;
            cachedFpsX = mainCanvas->width() - cachedFpsW - 4;
        }

        mainCanvas->fillRect(cachedFpsX, 4, cachedFpsW, 14, M5_COLOR_CARD_BG);
        mainCanvas->drawRect(cachedFpsX, 4, cachedFpsW, 14, M5_COLOR_CYAN);
        mainCanvas->setTextColor(M5_COLOR_GREEN);
        mainCanvas->setCursor(cachedFpsX + 4, 7);
        mainCanvas->print(cachedFpsBuf);
        
        // 4. 全屏四角极客视距折角框 (Corner Brackets ┌ ┐ └ ┘ 拓展至 240x135 屏幕最外角边缘)
        uint16_t bracketColor = M5_COLOR_CYAN;
        mainCanvas->drawFastHLine(3, 3, 10, bracketColor);
        mainCanvas->drawFastVLine(3, 3, 10, bracketColor);
        mainCanvas->drawFastHLine(227, 3, 10, bracketColor);
        mainCanvas->drawFastVLine(236, 3, 10, bracketColor);
        mainCanvas->drawFastHLine(3, 131, 10, bracketColor);
        mainCanvas->drawFastVLine(3, 122, 10, bracketColor);
        mainCanvas->drawFastHLine(227, 131, 10, bracketColor);
        mainCanvas->drawFastVLine(236, 122, 10, bracketColor);
        
        if (appState.overlayTimestamp > 0) {
            drawCaptureOverlay();
        }
        if (FilterManager::isFilterListOpen()) {
            drawFilterMenu();
        }
        mainCanvas->pushSprite(&M5Cardputer.Display, 0, 0);
        appState.consecutiveErrors = 0;
    } else {
        appState.consecutiveErrors++;
    }
    return drawSuccess;
}

void UIManager::renderTimelapse(int count, unsigned long last, unsigned long interval) {
    mainCanvas->fillSprite(M5_COLOR_DARK_BG);
    
    // Header Banner (霓虹粉紫底黑字)
    mainCanvas->fillRect(0, 0, 240, 22, M5_COLOR_PINK);
    mainCanvas->setTextColor(M5_COLOR_WHITE);
    mainCanvas->setTextSize(2); 
    mainCanvas->setCursor(45, 3);
    mainCanvas->println("TIMELAPSE");
    
    mainCanvas->setTextSize(1);
    mainCanvas->setTextColor(M5_COLOR_CYAN);
    mainCanvas->setCursor(20, 36);
    mainCanvas->print("Photos Captured: ");
    mainCanvas->setTextColor(M5_COLOR_YELLOW);
    mainCanvas->setTextSize(2);
    mainCanvas->printf("%d", count);
    
    long countdown = (long)interval - (long)(millis() - last);
    long cdSec = (countdown < 0 ? 0 : countdown / 1000);
    
    mainCanvas->setTextSize(1);
    mainCanvas->setTextColor(M5_COLOR_WHITE);
    mainCanvas->setCursor(20, 60);
    mainCanvas->printf("Next Shot In: %02ld s", cdSec);
    
    // 进度条 (Cyan 槽 + Green 进度条)
    int barX = 20, barY = 78, barW = 200, barH = 12;
    mainCanvas->fillRect(barX, barY, barW, barH, M5_COLOR_CARD_BG);
    mainCanvas->drawRect(barX, barY, barW, barH, M5_COLOR_CYAN);
    if (interval > 0) {
        long elapsed = (long)(millis() - last);
        if (elapsed > (long)interval) elapsed = interval;
        int fillW = (int)(elapsed * (barW - 4) / interval);
        if (fillW > 0) {
            mainCanvas->fillRect(barX + 2, barY + 2, fillW, barH - 4, M5_COLOR_GREEN);
        }
    }
    
    // Footer Exit Badge Banner (亮橙底黑字)
    mainCanvas->fillRect(15, 108, 210, 18, M5_COLOR_ORANGE);
    mainCanvas->setTextColor(M5_COLOR_BLACK);
    mainCanvas->setCursor(50, 113);
    mainCanvas->println("Hold BtnG0 to Exit");
    mainCanvas->pushSprite(&M5Cardputer.Display, 0, 0);
}

#include <JPEGDEC.h>

// Global/Static state variables for JPEGDEC callback access
static File outBmpFile;
static File inJpgFile;
static const uint16_t* filterLut = nullptr;
static int bmpWidth = 0;
static int bmpHeight = 0;
static int bmpRowSize = 0;
static int currentChunkY = 0;
static uint8_t* chunkRows[16] = {nullptr};

// Custom callbacks for JPEGDEC filesystem stream decoding
static void * myOpen(const char *szFilename, int32_t *pFileSize) {
    inJpgFile = SD.open(szFilename, FILE_READ);
    if (!inJpgFile) {
        serialPrintf("[FX] Callback open failed for: %s\n", szFilename);
        return nullptr;
    }
    *pFileSize = inJpgFile.size();
    serialPrintf("[FX] Callback open success: %s (%d bytes)\n", szFilename, (int)*pFileSize);
    return &inJpgFile;
}

static void myClose(void *pHandle) {
    if (inJpgFile) {
        inJpgFile.close();
        serialPrintf("[FX] Callback close file\n");
    }
}

static int32_t myRead(JPEGFILE *handle, uint8_t *buffer, int32_t length) {
    if (!inJpgFile) return 0;
    return inJpgFile.read(buffer, length);
}

static int32_t mySeek(JPEGFILE *handle, int32_t position) {
    if (!inJpgFile) return -1;
    if (!inJpgFile.seek(position)) return -1;
    return inJpgFile.position();
}

// JPEGDEC Callback function: assembles MCU blocks into 16-row chunks to allow high-speed sequential writes
static int JPEGDraw(JPEGDRAW *pDraw) {
    if (!outBmpFile) return 0;

    // Check if the current MCU y-coordinate has advanced past our 16-row memory buffer block
    while (pDraw->y >= currentChunkY + 16) {
        // Flush all 16 rows sequentially to the BMP file
        for (int i = 0; i < 16; i++) {
            if (chunkRows[i]) {
                outBmpFile.write(chunkRows[i], bmpRowSize);
            }
        }
        currentChunkY += 16;
        for (int i = 0; i < 16; i++) {
            if (chunkRows[i]) {
                memset(chunkRows[i], 0, bmpRowSize);
            }
        }
    }

    // Assemble this MCU block's pixel rows into our 16-row memory buffers
    for (int y = 0; y < pDraw->iHeight; y++) {
        int globalY = pDraw->y + y;
        int localY = globalY - currentChunkY;

        if (localY >= 0 && localY < 16 && chunkRows[localY]) {
            for (int x = 0; x < pDraw->iWidth; x++) {
                int globalX = pDraw->x + x;
                if (globalX >= bmpWidth) continue;

                uint16_t c = pDraw->pPixels[y * pDraw->iWidth + x];
                uint16_t fc = c;
                if (filterLut) {
                    uint16_t idx = ((c >> 1) & 0x7FE0) | (c & 0x001F);
                    fc = filterLut[idx];
                }

                // Write pixel in BGR888 format into the corresponding row buffer
                int bufIdx = globalX * 3;
                uint8_t* pixelPtr = chunkRows[localY] + bufIdx;
                pixelPtr[2] = ((fc >> 11) & 0x1F) << 3; // R
                pixelPtr[1] = ((fc >> 5)  & 0x3F) << 2; // G
                pixelPtr[0] = (fc         & 0x1F) << 3; // B
            }
        }
    }
    return 1; // Continue decoding
}

void UIManager::processAndSaveFilteredPhoto(const String& path, bool keepOriginal) {
    serialPrintf("[FX] Entering processAndSaveFilteredPhoto...\n");
    if (FilterManager::getFilter() == FILTER_NONE) {
        serialPrintf("[FX] No filter active, return\n");
        return;
    }

    // Allow the file system to completely flush the written JPG
    delay(150);

    // 零黑屏：不清空 LCD 屏幕，定格保留按下快门瞬间的画面，仅在画面下方叠加 FX Processing 贴纸胶囊
    M5Cardputer.Display.fillRect(40, 108, 160, 18, M5_COLOR_PINK);
    M5Cardputer.Display.drawRect(40, 108, 160, 18, M5_COLOR_YELLOW);
    M5Cardputer.Display.setTextColor(M5_COLOR_WHITE);
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setCursor(62, 113);
    M5Cardputer.Display.print("FX Processing...");

    // Print free system RAM
    serialPrintf("[FX] Free SRAM Heap (Canvases Active & Untouched): %d bytes\n", (int)ESP.getFreeHeap());

    bool saved = false;

    serialPrintf("[FX] Initializing JPEGDEC on heap...\n");
    JPEGDEC* jpeg = new JPEGDEC();
    if (!jpeg) {
        serialPrintf("[FX] Failed to allocate JPEGDEC on heap\n");
    } else {
        serialPrintf("[FX] Opening input JPG file stream: %s\n", path.c_str());
        if (jpeg->open(path.c_str(), myOpen, myClose, myRead, mySeek, JPEGDraw)) {
            int W = jpeg->getWidth();
            int H = jpeg->getHeight();
            serialPrintf("[FX] JPEGDEC parsed dimensions: %dx%d\n", W, H);

            String pfx = path;
            pfx.replace(".jpg", "_FX.bmp");

            serialPrintf("[FX] Opening output BMP file: %s\n", pfx.c_str());
            outBmpFile = SD.open(pfx, FILE_WRITE);
            if (!outBmpFile) {
                serialPrintf("[FX] Failed to create output BMP\n");
            } else {
                bmpWidth = W;
                bmpHeight = H;
                bmpRowSize = (W * 3 + 3) & ~3; // 4-byte aligned BMP row size
                
                // Borrow the inactive mainCanvas pixel buffer (64.8KB contiguous SRAM) 
                // to store our 16 row buffers (needs 30.7KB) during decoding.
                // This eliminates any heap allocation or fragmentation risk.
                uint8_t* basePtr = (uint8_t*)mainCanvas->getBuffer();
                if (!basePtr) {
                    serialPrintf("[FX] Critical Error: mainCanvas buffer is null!\n");
                } else {
                    serialPrintf("[FX] Successfully borrowed mainCanvas buffer at %p\n", basePtr);
                    for (int i = 0; i < 16; i++) {
                        chunkRows[i] = basePtr + i * bmpRowSize;
                        memset(chunkRows[i], 0, bmpRowSize);
                    }

                    filterLut = FilterManager::getCurrentLUT();
                    currentChunkY = 0;

                    int fileSize = 54 + bmpRowSize * H;

                    serialPrintf("[FX] Writing BMP Headers...\n");
                    // 1. BMP Header (14 bytes)
                    uint8_t header[14] = {'B', 'M', 0,0,0,0, 0,0, 0,0, 54,0,0,0};
                    *(uint32_t*)(header + 2) = fileSize;
                    outBmpFile.write(header, 14);

                    // 2. Info Header (40 bytes)
                    uint8_t info[40] = {40,0,0,0, 0,0,0,0, 0,0,0,0, 1,0, 24,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0};
                    *(int32_t*)(info + 4) = W;
                    *(int32_t*)(info + 8) = -H; // Negative height = Top-down BMP
                    outBmpFile.write(info, 40);

                    serialPrintf("[FX] Starting JPEG decoding...\n");
                    if (jpeg->decode(0, 0, 0)) {
                        serialPrintf("[FX] Flush remaining rows...\n");
                        int remainingRows = H - currentChunkY;
                        for (int i = 0; i < remainingRows; i++) {
                            if (chunkRows[i]) {
                                outBmpFile.write(chunkRows[i], bmpRowSize);
                            }
                        }
                        saved = true;
                        serialPrintf("[FX] Successfully saved %dx%d BMP via JPEGDEC sequential streaming\n", W, H);
                    } else {
                        serialPrintf("[FX] JPEGDEC decode failed\n");
                    }

                    // Reset our borrowed pointers without free()
                    for (int i = 0; i < 16; i++) {
                        chunkRows[i] = nullptr;
                    }
                }
                outBmpFile.close();
            }
            jpeg->close();
        } else {
            serialPrintf("[FX] JPEGDEC open failed (file callbacks)\n");
        }
        delete jpeg;
        jpeg = nullptr;
    }

    if (saved && !keepOriginal) {
        SD.remove(path.c_str());
    }

    if (!saved) {
        serialPrintf("[FX] All processing methods failed, keeping original JPG\n");
    }

    // Clean mainCanvas content to prevent stale preview pixels
    mainCanvas->clear();

    snprintf(appState.overlayMsg, sizeof(appState.overlayMsg), saved ? "FX Saved!" : "FX Failed");
    appState.overlayTimestamp = millis();
}

void UIManager::clear() {
    M5Cardputer.Display.clearDisplay();
}
