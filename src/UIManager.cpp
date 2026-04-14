#include "UIManager.h"
#include <ArduinoJson.h>
#include <SD.h>
#include <vector>
#include "StorageManager.h"
#include "MjpegParser.h"
#include "FilterManager.h"
#include "InputHandler.h"

// 外部引用或定义 Canvas
M5Canvas canvas(&M5Cardputer.Display);
M5Canvas mainCanvas(&M5Cardputer.Display);

void UIManager::init() {
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.fillScreen(BLACK);
    
    // 160x120 视频中间层 (硬件 1/2 解码)
    canvas.createSprite(160, 120);
    
    // 240x135 完整物理分辨率复合缓冲区 (双缓冲核心)
    mainCanvas.createSprite(240, 135);
}

void UIManager::displayLine(String line, bool reset) {
    static int currentLine = 0;
    if (reset) {
        currentLine = 0;
        M5Cardputer.Display.fillScreen(BLACK);
    }
    M5Cardputer.Display.setCursor(10, 10 + currentLine * 12);
    M5Cardputer.Display.println(line);
    currentLine++;
    if (currentLine > 15) {
        currentLine = 0;
        M5Cardputer.Display.fillScreen(BLACK);
    }
}

void UIManager::drawCaptureOverlay() {
    if (appState.overlayTimestamp > 0) {
        if (millis() - appState.overlayTimestamp < 5000) {
            mainCanvas.setTextColor(TFT_WHITE);
            mainCanvas.setTextSize(1);
            mainCanvas.setCursor(6, 122);
            mainCanvas.print(appState.overlayMsg);
        } else {
            appState.overlayTimestamp = 0;
        }
    }
}

void UIManager::showStatus(const String& statusJson) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, statusJson);
    
    std::vector<String> lines;
    lines.push_back("--- [ CAMERA STATUS ] ---");
    
    if (error) {
        lines.push_back("Error: JSON Parse Fail");
    } else {
        auto add = [&](const char* label, const char* key) {
            char b[64];
            snprintf(b, sizeof(b), "%-12s: %s", label, doc[key].as<String>().c_str());
            lines.push_back(String(b));
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
    lines.push_back("-------------------------");
    lines.push_back("ESC/Enter: Back");

    int offset = 0;
    const int visible = 11;

    while (true) {
        mainCanvas.fillSprite(UI_COLOR_BG);
        mainCanvas.setTextColor(UI_COLOR_TITLE);
        mainCanvas.setTextSize(1);
        mainCanvas.setCursor(10, 5);
        mainCanvas.println("--- [ CAMERA STATUS ] ---");
        
        for (int i = 0; i < visible && (offset + i) < (int)lines.size(); i++) {
            mainCanvas.setCursor(10, 20 + i * 12);
            mainCanvas.setTextColor(UI_COLOR_TEXT);
            mainCanvas.println(lines[offset + i]);
        }
        
        if (lines.size() > (size_t)visible) {
            int bh = 100, th = bh * visible / lines.size();
            // 精准映射公式：(当前位置) / (最大位置) * 可移动区间
            int ty = 20 + (bh - th) * offset / (lines.size() - visible);
            mainCanvas.fillRect(235, 20, 2, bh, UI_COLOR_BAR_BG);
            mainCanvas.fillRect(235, ty, 2, th, UI_COLOR_ACCENT);
        }

        mainCanvas.pushSprite(&M5Cardputer.Display, 0, 0);
        M5Cardputer.update();
        InputEvent ev = InputHandler::handle();
        
        if (ev == EVENT_BRIGHTNESS_UP) {
            if (offset > 0) offset--;
        } else if (ev == EVENT_BRIGHTNESS_DOWN) {
            if (offset < (int)lines.size() - visible) offset++;
        } else if (ev == EVENT_STATUS || ev == EVENT_RESTART) {
            break;
        } else if (ev != EVENT_NONE && ev != EVENT_BRIGHTNESS_UP && ev != EVENT_BRIGHTNESS_DOWN) {
            // 只有非滚动按键才视为退出指令
            break;
        }
        delay(10);
    }
}

void UIManager::showWiFiPortal() {
    mainCanvas.fillSprite(TFT_BLACK);
    mainCanvas.setTextColor(TFT_CYAN);
    mainCanvas.setTextSize(2);
    mainCanvas.setCursor(10, 5);
    mainCanvas.println("WiFi TRANSFER");

    mainCanvas.setTextSize(1);
    mainCanvas.setTextColor(TFT_WHITE);
    mainCanvas.setCursor(10, 35);
    mainCanvas.println("1. Connect: Cardputer-Cam");
    mainCanvas.setCursor(10, 50);
    mainCanvas.println("2. No password required");
    mainCanvas.setCursor(10, 65);
    mainCanvas.println("3. URL: http://192.168.4.1");
    mainCanvas.setCursor(10, 85);
    mainCanvas.setTextColor(TFT_YELLOW);
    mainCanvas.println("Press 'W' to exit & resume");
    
    mainCanvas.pushSprite(&M5Cardputer.Display, 0, 0);
}

void UIManager::showHelp() {
    std::vector<String> lines = {
        "--- [ CAMERA CONTROLS ] ---",
        "BtnA: Capture Photo / Burst",
        "T   : Timelapse Mode",
        "W   : WiFi File Server",
        "R   : Restart Device",
        "`   : Camera Status",
        "H   : Help Menu",
        "---------------------------",
        "7   : Filter - GameBoy",
        "8   : Filter - Pixelate",
        "9   : Filter - Glitch",
        "; / . : Brightness Up/Down",
        "[ / ] : Saturation Up/Down",
        "_ / = : Sharpness Up/Down",
        ", / / : Contrast Up/Down",
        "---------------------------",
        "ESC/H: Return to Finder"
    };

    int offset = 0;
    const int visible = 11;

    while (true) {
        mainCanvas.fillSprite(UI_COLOR_BG);
        mainCanvas.setTextColor(UI_COLOR_TITLE);
        mainCanvas.setTextSize(1);
        mainCanvas.setCursor(10, 5);
        mainCanvas.println("--- [ CAMERA HELP ] ---");
        
        for (int i = 0; i < visible && (offset + i) < (int)lines.size(); i++) {
            mainCanvas.setCursor(10, 20 + i * 12);
            mainCanvas.setTextColor(UI_COLOR_TEXT);
            mainCanvas.println(lines[offset + i]);
        }

        if (lines.size() > (size_t)visible) {
            int bh = 100, th = bh * visible / lines.size();
            int ty = 20 + (bh - th) * offset / (lines.size() - visible);
            mainCanvas.fillRect(235, 20, 2, bh, UI_COLOR_BAR_BG);
            mainCanvas.fillRect(235, ty, 2, th, UI_COLOR_ACCENT);
        }

        mainCanvas.pushSprite(&M5Cardputer.Display, 0, 0);
        M5Cardputer.update();
        InputEvent ev = InputHandler::handle();
        
        if (ev == EVENT_BRIGHTNESS_UP) {
            if (offset > 0) offset--;
        } else if (ev == EVENT_BRIGHTNESS_DOWN) {
            if (offset < (int)lines.size() - visible) offset++;
        } else if (ev == EVENT_HELP || ev == EVENT_RESTART) {
            break;
        } else if (ev != EVENT_NONE && ev != EVENT_BRIGHTNESS_UP && ev != EVENT_BRIGHTNESS_DOWN) {
            // 只有非滚动按键才视为退出指令
            break;
        }
        delay(10);
    }
}

bool UIManager::renderStream() {
    bool drawSuccess = canvas.drawJpg(appState.networkBuffer, appState.networkSize, 0, 0, 0, 0, 0, 0, 0.5f);
    if (drawSuccess && FilterManager::getFilter() != FILTER_NONE) {
        FilterManager::applyToCanvas(canvas, 0, 0, canvas.width(), canvas.height());
    }
    if (drawSuccess) {
        canvas.pushRotateZoom(&mainCanvas, mainCanvas.width()/2, mainCanvas.height()/2, 0, 1.5f, 1.5f);
        mainCanvas.setTextColor(TFT_WHITE);
        mainCanvas.setTextSize(1);
        
        // B. 准备并绘制 HUD 信息
        static uint32_t lastOverlayUpdate = 0;
        static uint32_t cachedUsedKB = 0;
        static uint32_t cachedTotalKB = 0;
        if (millis() - lastOverlayUpdate >= 1000) {
            lastOverlayUpdate = millis();
            cachedUsedKB = (ESP.getHeapSize() - ESP.getFreeHeap()) / 1024;
            cachedTotalKB = ESP.getHeapSize() / 1024;
        }

        FilterMode fm = FilterManager::getFilter();
        const char* filterNames[] = { "", "GB", "PIX", "GLT" };
        char buf[64];
        if (fm != FILTER_NONE) {
            snprintf(buf, sizeof(buf), "[%s] FPS:%.1f %u/%uK", filterNames[(int)fm], currentFps, cachedUsedKB, cachedTotalKB);
        } else {
            snprintf(buf, sizeof(buf), "FPS:%.1f %u/%uK", currentFps, cachedUsedKB, cachedTotalKB);
        }
        
        int textWidth = strlen(buf) * 6;
        mainCanvas.setCursor(mainCanvas.width() - textWidth - 2, 2);
        mainCanvas.print(buf);
        
        drawCaptureOverlay();
        mainCanvas.pushSprite(&M5Cardputer.Display, 0, 0);
        appState.consecutiveErrors = 0;
    } else {
        appState.consecutiveErrors++;
    }
    return drawSuccess;
}

void UIManager::renderTimelapse(int count, unsigned long last, unsigned long interval) {
    mainCanvas.fillSprite(UI_COLOR_BG);
    mainCanvas.setTextColor(UI_COLOR_TITLE);
    mainCanvas.setTextSize(2); 
    mainCanvas.setCursor(15, 15);
    mainCanvas.println("Timelapse");
    
    mainCanvas.setTextSize(1);
    mainCanvas.setTextColor(UI_COLOR_TEXT);
    mainCanvas.setCursor(15, 45);
    mainCanvas.printf("Photos: %d", count);
    
    long countdown = (long)interval - (long)(millis() - last);
    mainCanvas.setCursor(15, 60);
    mainCanvas.printf("Next Photo: %02ld s", (countdown < 0 ? 0 : countdown / 1000));
    
    mainCanvas.setCursor(15, 115);
    mainCanvas.setTextColor(UI_COLOR_ACCENT);
    mainCanvas.println("Hold BtnA to Exit");
    mainCanvas.pushSprite(&M5Cardputer.Display, 0, 0);
}

void UIManager::processAndSaveFilteredPhoto(const String& path) {
    if (FilterManager::getFilter() == FILTER_NONE) return;
    mainCanvas.deleteSprite();
    canvas.deleteSprite();
    delay(100);

    M5Cardputer.Display.fillScreen(BLACK);
    M5Cardputer.Display.setCursor(6, 60);
    M5Cardputer.Display.setTextColor(TFT_YELLOW);
    M5Cardputer.Display.print("FX Processing...");

    M5Canvas fx(&M5Cardputer.Display);
    if (fx.createSprite(240, 180)) {
        File f = SD.open(path);
        if (f) {
            size_t sz = f.size();
            uint8_t* b = (uint8_t*)malloc(sz);
            if (b) {
                f.read(b, sz);
                if (fx.drawJpg(b, sz, 0, 0, 240, 180, 0, 0, JPEG_DIV_2)) {
                    FilterManager::applyToCanvas(fx, 0, 0, 240, 180);
                    String pfx = path; pfx.replace(".jpg", "_FX.bmp");
                    StorageManager::saveCanvasAsBmp(fx, pfx.c_str());
                }
                free(b);
            }
            f.close();
        }
        fx.deleteSprite();
    }
    
    mainCanvas.createSprite(240, 135);
    canvas.createSprite(160, 120);
    snprintf(appState.overlayMsg, sizeof(appState.overlayMsg), "FX Saved!");
    appState.overlayTimestamp = millis();
}

void UIManager::clear() {
    M5Cardputer.Display.clearDisplay();
}
