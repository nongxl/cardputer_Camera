#include "StorageManager.h"
#include <SPI.h>
#include "USB.h"
#include "USBMSC.h"

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

bool StorageManager::saveCanvasAsBmp(M5Canvas& canvas, const char* filename) {
    int w = canvas.width();
    int h = canvas.height();
    int rowSize = (w * 3 + 3) & ~3; // 每行需 4 字节对齐
    int dataSize = rowSize * h;
    int fileSize = 54 + dataSize;
    
    File file = SD.open(filename, FILE_WRITE);
    if (!file) return false;

    // 1. BMP Header (14 bytes)
    uint8_t header[14] = {'B', 'M', 0,0,0,0, 0,0, 0,0, 54,0,0,0};
    *(uint32_t*)(header + 2) = fileSize;
    file.write(header, 14);

    // 2. Info Header (40 bytes)
    uint8_t info[40] = {40,0,0,0, 0,0,0,0, 0,0,0,0, 1,0, 24,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0};
    *(int32_t*)(info + 4) = w;
    *(int32_t*)(info + 8) = h; // 正值表示自底向上
    file.write(info, 40);

    // 3. Pixel Data (BGR888, Bottom-up)
    uint8_t* rowBuf = (uint8_t*)malloc(rowSize);
    if (!rowBuf) { file.close(); return false; }
    
    for (int y = h - 1; y >= 0; y--) {
        memset(rowBuf, 0, rowSize);
        for (int x = 0; x < w; x++) {
            uint16_t c = canvas.readPixel(x, y);
            // RGB565 -> BGR888
            rowBuf[x * 3 + 2] = ((c >> 11) & 0x1F) << 3; // R
            rowBuf[x * 3 + 1] = ((c >> 5)  & 0x3F) << 2; // G
            rowBuf[x * 3 + 0] = (c         & 0x1F) << 3; // B
        }
        file.write(rowBuf, rowSize);
    }
    
    free(rowBuf);
    file.close();
    return true;
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

// ── USB Mass Storage Class (MSC) 实现 ─────────────────────────
static USBMSC msc;

// 当电脑读取 SD 卡扇区时的回调
static int32_t onRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
    uint32_t sector_count = bufsize / 512;
    for (uint32_t i = 0; i < sector_count; i++) {
        if (!SD.readRAW((uint8_t*)buffer + i * 512, lba + i)) {
            return -1;
        }
    }
    return bufsize;
}

// 当电脑写入 SD 卡扇区时的回调
static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
    uint32_t sector_count = bufsize / 512;
    for (uint32_t i = 0; i < sector_count; i++) {
        if (!SD.writeRAW(buffer + i * 512, lba + i)) {
            return -1;
        }
    }
    return bufsize;
}

void StorageManager::startUSBMSC() {
    if (!isSDInitialized) return;
    
    msc.onRead(onRead);
    msc.onWrite(onWrite);
    msc.mediaPresent(true);
    // SD.cardSize() 返回字节大小，除以 512 得到总扇区数
    msc.begin(SD.cardSize() / 512, 512);
    USB.begin();
    serialPrintf("[USB] Mass Storage device initialized and started.\n");
}

void StorageManager::stopUSBMSC() {
    msc.mediaPresent(false);
    msc.end();
    
    // 强制卸载重新挂载 SD 卡，保证 PC 写入的最新 FAT 分区变动在 Cardputer 端完全同步刷新
    SD.end();
    delay(200);
    StorageManager::init();
    serialPrintf("[USB] Mass Storage stopped. SD card remounted.\n");
}
