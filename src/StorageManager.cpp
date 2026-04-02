#include "StorageManager.h"
#include <SPI.h>

bool StorageManager::init() {
    const int SD_CS = 12;
    const int SD_SCK = 40;
    const int SD_MISO = 39;
    const int SD_MOSI = 14;
    
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    
    if (!SD.begin(SD_CS)) {
        Serial.println("SD card initialization failed!");
        isSDInitialized = false;
        return false;
    }
    
    isSDInitialized = true;
    Serial.println("SD card initialized successfully!");
    
    if (!SD.exists("/images")) {
        if (SD.mkdir("/images")) {
            Serial.println("Created /images directory");
        } else {
            Serial.println("Failed to create /images directory");
        }
    }
    
    return true;
}

bool StorageManager::savePhoto(const char* filename, uint8_t* buffer, size_t size) {
    File file = SD.open(filename, FILE_WRITE);
    if (!file) {
        Serial.printf("Failed to open file for writing: %s\n", filename);
        return false;
    }
    size_t written = file.write(buffer, size);
    file.close();
    return (written == size);
}

bool StorageManager::saveCameraStatus(const String& statusJson) {
    File file = SD.open("/camera_status.json", FILE_WRITE);
    if (file) {
        size_t written = file.print(statusJson);
        file.close();
        return (written > 0);
    }
    return false;
}

bool StorageManager::loadCameraStatus(JsonDocument& doc) {
    File file = SD.open("/camera_status.json");
    if (!file) return false;
    
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    return !error;
}

bool StorageManager::createDirectory(const char* path) {
    if (!SD.exists(path)) {
        return SD.mkdir(path);
    }
    return true;
}

bool StorageManager::exists(const char* path) {
    return SD.exists(path);
}
