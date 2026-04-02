#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include "Global.h"

class UIManager {
public:
    static void init();
    static void displayLine(String line, bool reset = false);
    static void drawCaptureOverlay();
    static void showStatus(const String& statusJson);
    static void showHelp();
    static bool renderStream();
    static void renderTimelapse(int photoCount, unsigned long lastShotTime, unsigned long interval);
    static void clear();
};

extern M5Canvas canvas;

#endif // UI_MANAGER_H
