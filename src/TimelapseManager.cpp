#include "TimelapseManager.h"
#include "CameraClient.h"
#include "StorageManager.h"
#include "UIManager.h"
#include <time.h>

String TimelapseManager::currentDir = "";
int TimelapseManager::photoCount = 0;
unsigned long TimelapseManager::lastShotTime = 0;
unsigned long TimelapseManager::startTime = 0;
unsigned long TimelapseManager::interval = 5000;
unsigned long TimelapseManager::lastActionTime = 0;
bool TimelapseManager::isScreenOff = false;

bool TimelapseManager::start() {
    if (!StorageManager::isReady()) return false;
    
    // Create Dir
    time_t now = time(nullptr);
    struct tm *t = localtime(&now);
    char dirname[64];
    sprintf(dirname, "/images/TL_%04d%02d%02d_%02d%02d%02d", 
            t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
    
    if (!StorageManager::createDirectory(dirname)) return false;
    currentDir = dirname;
    
    CameraClient::setResolution(CAMERA_RESOLUTION_TIMELAPSE);
    CameraClient::setQuality(2);
    
    isTimelapseMode = true;
    photoCount = 0;
    lastShotTime = millis();
    startTime = millis();
    lastActionTime = millis();
    isScreenOff = false;
    
    UIManager::clear();
    UIManager::displayLine("Timelapse Mode Started");
    delay(1000);
    return true;
}

void TimelapseManager::stop() {
    isTimelapseMode = false;
    isScreenOff = false;
    if (isScreenOff) M5Cardputer.Display.wakeup();
    
    CameraClient::setResolution(CAMERA_RESOLUTION_LOW);
    CameraClient::setQuality(CAMERA_QUALITY_STREAM);
    appState.isRestartStream = true;
}

void TimelapseManager::update() {
    if (!isTimelapseMode) return;
    
    // Check Input for Exit or Wakeup
    if (M5Cardputer.Keyboard.isChange() || M5Cardputer.BtnA.wasPressed()) {
        lastActionTime = millis();
        if (isScreenOff) {
            isScreenOff = false;
            M5Cardputer.Display.wakeup();
        } else if (M5Cardputer.BtnA.wasPressed()) {
            stop();
            return;
        }
    }
    
    // Auto Screen Off
    if (!isScreenOff && millis() - lastActionTime > 60000) {
        isScreenOff = true;
        M5Cardputer.Display.sleep();
    }
    
    // Display
    if (!isScreenOff) {
        UIManager::renderTimelapse(photoCount, lastShotTime, interval);
    }
    
    // Trigger Capture
    if (millis() - lastShotTime >= interval) {
        capture();
    }
}

bool TimelapseManager::capture() {
    char filename[128];
    sprintf(filename, "%s/IMG_%04d.jpg", currentDir.c_str(), photoCount + 1);
    
    CameraClient::triggerCapture();
    delay(500); // Wait for camera
    if (CameraClient::downloadPhoto(filename)) {
        photoCount++;
        lastShotTime = millis();
        return true;
    }
    lastShotTime = millis(); // Reset even if failed to avoid loop
    return false;
}
