#include "UIManager.h"
#include <ArduinoJson.h>
#include "MjpegParser.h"

M5Canvas canvas(&M5Cardputer.Display);

void UIManager::init() {
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.fillScreen(BLACK);
    canvas.createSprite(M5Cardputer.Display.width(), M5Cardputer.Display.height());
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
            canvas.setTextColor(TFT_WHITE);
            canvas.setTextSize(1);
            canvas.setCursor(10, 120);
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
        "ESC : Camera Status",
        "H   : Help Menu",
        "0-6 : Effects"
    };
    
    for (int i = 0; i < 6; i++) {
        M5Cardputer.Display.setCursor(10, 35 + i * 12);
        M5Cardputer.Display.println(helpText[i]);
    }
}

bool UIManager::renderStream() {
    int jpegW = 0, jpegH = 0;
    int dispW = M5Cardputer.Display.width();
    int dispH = M5Cardputer.Display.height();
    bool drawSuccess = false;
    
    if (MjpegParser::parseJpegSize(appState.networkBuffer, appState.networkSize, jpegW, jpegH)) {
        int drawX = (dispW - jpegW) / 2;
        int drawY = (dispH - jpegH) / 2;
        drawSuccess = canvas.drawJpg(appState.networkBuffer, appState.networkSize, drawX, drawY);
    }
    
    // FPS & Memory Info Overlay
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
    canvas.setCursor(115, 2);
    canvas.printf("FPS:%.1f %u/%uKB", currentFps, cachedUsedKB, cachedTotalKB);
    
    drawCaptureOverlay();
    
    if (drawSuccess) {
        canvas.pushSprite(0, 0);
        appState.consecutiveErrors = 0;
    } else {
        appState.consecutiveErrors++;
        // 可选：在这里添加串口打印错误详情
    }
    
    return drawSuccess;
}

void UIManager::renderTimelapse(int photoCount, unsigned long lastShotTime, unsigned long interval) {
    canvas.fillScreen(BLACK);
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    canvas.setTextSize(2);
    canvas.setCursor(10, 5);
    canvas.println("Timelapse");
    
    canvas.setTextSize(1);
    canvas.setCursor(10, 35);
    canvas.printf("Photos: %d", photoCount);
    
    unsigned long timeSinceLastShot = millis() - lastShotTime;
    long countdown = (long)interval - (long)timeSinceLastShot;
    if (countdown < 0) countdown = 0;
    
    canvas.setCursor(10, 55);
    if (timeSinceLastShot >= interval) {
        canvas.setTextColor(TFT_YELLOW);
        canvas.println("Status: Capturing...");
    } else {
        canvas.setTextColor(TFT_CYAN);
        canvas.printf("Next Photo: %02ld s", countdown / 1000);
    }
    
    canvas.setCursor(10, 115);
    canvas.setTextColor(TFT_DARKGREY);
    canvas.println("Hold BtnA to Exit");
    
    canvas.pushSprite(0, 0);
}

void UIManager::clear() {
    M5Cardputer.Display.clearDisplay();
}
