#include "UIManager.h"
#include <ArduinoJson.h>
#include "MjpegParser.h"
#include "FilterManager.h"

M5Canvas canvas(&M5Cardputer.Display);

void UIManager::init() {
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.fillScreen(BLACK);
    canvas.createSprite(160, 120);
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
            // 回归画布绘制，解决闪烁和撕裂
            canvas.setTextColor(TFT_WHITE);
            canvas.setTextSize(1);
            // 画布坐标系底部 (160x120)，预留 20 像素的缩放裁剪区
            canvas.setCursor(6, 92);
            canvas.print(appState.overlayMsg);
        } else {
            appState.overlayTimestamp = 0;
        }
    }
}

void UIManager::showStatus(const String& statusJson) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, statusJson);
    
    M5Cardputer.Display.clearDisplay();
    M5Cardputer.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
    M5Cardputer.Display.setTextSize(2);
    M5Cardputer.Display.setCursor(10, 5);
    M5Cardputer.Display.println("Camera Status");
    
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    
    if (error) {
        M5Cardputer.Display.setCursor(10, 40);
        M5Cardputer.Display.println("JSON Parse Error");
    } else {
        int y = 35;
        auto printParam = [&](const char* label, const char* key) {
            M5Cardputer.Display.setCursor(10, y);
            M5Cardputer.Display.printf("%-12s: %s", label, doc[key].as<String>().c_str());
            y += 15;
        };
        printParam("Resolution", "framesize");
        printParam("Quality", "quality");
        printParam("Brightness", "brightness");
        printParam("Contrast", "contrast");
        printParam("Saturation", "saturation");
    }
    
    M5Cardputer.Display.setCursor(10, 115);
    M5Cardputer.Display.setTextColor(TFT_DARKGREY);
    M5Cardputer.Display.println("Press ANY Key to Return");
}

void UIManager::showHelp() {
    M5Cardputer.Display.clearDisplay();
    M5Cardputer.Display.setTextColor(TFT_GREEN, TFT_BLACK);
    M5Cardputer.Display.setTextSize(2);
    M5Cardputer.Display.setCursor(10, 5);
    M5Cardputer.Display.println("Controls Help");
    
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    
    const char* helpText[] = {
        "BtnA: Capture Photo",
        "T   : Timelapse Mode",
        "R   : Restart Device",
        "` : Camera Status",
        "H   : Help Menu",
        "0-6 : Camera Effects",
        "7   : Filter:GameBoy",
        "8   : Filter:Pixelate",
        "9   : Filter:Glitch",
        ";/. : Brightness",
        ",// : Contrast",
        "[/] : Saturation",
        "_/= : Sharpness"
    };
    
    for (int i = 0; i < 13; i++) {
        M5Cardputer.Display.setCursor(10, 35 + i * 10);
        M5Cardputer.Display.println(helpText[i]);
    }
}

bool UIManager::renderStream() {
    bool drawSuccess = false;
    
    // 步骤1：JPEG 解码至画布 (160x120)
    drawSuccess = canvas.drawJpg(appState.networkBuffer, appState.networkSize, 0, 0, 0, 0, 0, 0, 0.5f);
    
    // 步骤2：滤镜处理
    if (drawSuccess && FilterManager::getFilter() != FILTER_NONE) {
        FilterManager::applyToCanvas(canvas, 0, 0, canvas.width(), canvas.height());
    }
    
    // 步骤3：在画板上先行叠加 HUD (解决闪烁)
    static uint32_t lastOverlayUpdate = 0;
    static uint32_t cachedUsedKB = 0;
    static uint32_t cachedTotalKB = 0;
    if (millis() - lastOverlayUpdate >= 1000) {
        lastOverlayUpdate = millis();
        cachedUsedKB = (ESP.getHeapSize() - ESP.getFreeHeap()) / 1024;
        cachedTotalKB = ESP.getHeapSize() / 1024;
    }

    canvas.setTextColor(TFT_WHITE);
    canvas.setTextSize(1);
    
    FilterMode fm = FilterManager::getFilter();
    const char* filterNames[] = { "", "GB", "PIX", "GLT" };
    
    char buf[64];
    if (fm != FILTER_NONE) {
        snprintf(buf, sizeof(buf), "[%s] FPS:%.1f %u/%uK", filterNames[(int)fm], currentFps, cachedUsedKB, cachedTotalKB);
    } else {
        snprintf(buf, sizeof(buf), "FPS:%.1f %u/%uK", currentFps, cachedUsedKB, cachedTotalKB);
    }
    
    // 160 画布右对齐 (TextSize 1 每个字宽 6 像素)
    int textWidth = strlen(buf) * 6;
    canvas.setCursor(160 - textWidth - 4, 18);
    canvas.print(buf);
    
    // 叠加拍摄状态提示
    drawCaptureOverlay();
    
    // 步骤4：唯一一次推屏投射 (1.5x 缩放)
    if (drawSuccess) {
        canvas.pushRotateZoom(&M5Cardputer.Display, 
                               M5Cardputer.Display.width()/2, 
                               M5Cardputer.Display.height()/2, 
                               0, 1.5f, 1.5f);
        appState.consecutiveErrors = 0;
    } else {
        appState.consecutiveErrors++;
    }
    
    return drawSuccess;
}

void UIManager::renderTimelapse(int photoCount, unsigned long lastShotTime, unsigned long interval) {
    // 全过程在画布上构建
    canvas.fillScreen(BLACK);
    
    canvas.setTextColor(TFT_WHITE);
    canvas.setTextSize(2); // 标题可以使用稍大尺寸
    canvas.setCursor(10, 20);
    canvas.println("Timelapse");
    
    canvas.setTextSize(1);
    canvas.setCursor(10, 45);
    canvas.printf("Photos: %d", photoCount);
    
    unsigned long timeSinceLastShot = millis() - lastShotTime;
    long countdown = (long)interval - (long)timeSinceLastShot;
    if (countdown < 0) countdown = 0;
    
    canvas.setCursor(10, 60);
    if (timeSinceLastShot >= interval) {
        canvas.setTextColor(TFT_YELLOW);
        canvas.println("Status: Capturing...");
    } else {
        canvas.setTextColor(TFT_CYAN);
        canvas.printf("Next Photo: %02ld s", countdown / 1000);
    }
    
    canvas.setCursor(10, 95);
    canvas.setTextColor(TFT_DARKGREY);
    canvas.println("Hold BtnA to Exit");
    
    // 最后整屏一次性推送到 Display，彻底解决刷新撕裂
    canvas.pushRotateZoom(&M5Cardputer.Display, 
                           M5Cardputer.Display.width()/2, 
                           M5Cardputer.Display.height()/2, 
                           0, 1.5f, 1.5f);
}

void UIManager::clear() {
    M5Cardputer.Display.clearDisplay();
}
