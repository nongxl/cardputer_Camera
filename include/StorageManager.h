#ifndef STORAGE_MANAGER_H
#define STORAGE_MANAGER_H

#include "Global.h"
#include <SD.h>
#include <SPI.h>
#include <ArduinoJson.h>

class StorageManager {
public:
    static bool init();
    static bool isReady() { return isSDInitialized; }
    static bool savePhoto(const char* filename, uint8_t* buffer, size_t size);
    static bool saveCameraStatus(const String& statusJson);
    static bool loadCameraStatus(JsonDocument& doc);
    static bool createDirectory(const char* path);
    static bool exists(const char* path);
};

#endif // STORAGE_MANAGER_H
