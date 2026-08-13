#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include "Global.h"

class UIManager {
public:
    static void init();
    static void displayLine(String line, bool reset = false);
    static void drawCaptureOverlay();
    static void showStatus(const String& statusJson);
    static void showWiFiPortal();
    static void showUsbPortal();
    static void showHelp();
    static bool renderStream();
    static void renderTimelapse(int photoCount, unsigned long lastShotTime, unsigned long interval);
    static void processAndSaveFilteredPhoto(const String& originalPath);
    static void clear();
};

extern M5Canvas canvas;
extern M5Canvas mainCanvas;

#endif // UI_MANAGER_H
