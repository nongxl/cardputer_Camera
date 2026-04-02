#include "CameraClient.h"
#include <HTTPClient.h>
#include <time.h>
#include "StorageManager.h"

bool CameraClient::setParameter(const String& param, int value) {
    HTTPClient http;
    String url = String("http://192.168.4.1/api/v1/control?var=") + param + String("&val=") + value;
    http.begin(url);
    http.addHeader("User-Agent", "M5Cardputer");
    int code = http.GET();
    http.end();
    return (code == 200);
}

bool CameraClient::setResolution(int resolution) {
    serialPrintf("Setting camera resolution to %d...\n", resolution);
    bool success = setParameter("framesize", resolution);
    if (success) {
        appState.sizeCached = false;
        appState.cachedImgWidth = 0;
        appState.cachedImgHeight = 0;
    }
    return success;
}

bool CameraClient::setQuality(int quality) {
    serialPrintf("Setting camera quality to %d...\n", quality);
    return setParameter("quality", quality);
}

bool CameraClient::setSpecialEffect(int effect) {
    serialPrintf("Setting camera effect to %d...\n", effect);
    return setParameter("special_effect", effect);
}

bool CameraClient::getStatus(String& response) {
    HTTPClient http;
    http.begin("http://192.168.4.1/api/v1/status");
    http.addHeader("User-Agent", "M5Cardputer");
    http.setTimeout(15000);
    int code = http.GET();
    if (code == 200) {
        response = http.getString();
    }
    http.end();
    return (code == 200);
}

bool CameraClient::triggerCapture() {
    HTTPClient http;
    http.begin("http://192.168.4.1/api/v1/capture");
    http.addHeader("User-Agent", "M5Cardputer");
    http.setTimeout(5000);
    int code = http.GET();
    http.end();
    return (code == 200);
}

bool CameraClient::downloadPhoto(const String& filename, bool isBurst, int burstIndex) {
    if (!isSDInitialized) return false;
    
    HTTPClient http;
    http.begin("http://192.168.4.1/api/v1/capture");
    http.addHeader("User-Agent", "M5Cardputer");
    http.setTimeout(10000);
    int code = http.GET();
    
    if (code != 200) {
        http.end();
        return false;
    }
    
    // 我们需要把文件保存到 SD 卡，这里调用 StorageManager。
    // 但是这里 http 是以流的形式读取的。
    WiFiClient* s = http.getStreamPtr();
    File file = SD.open(filename, FILE_WRITE);
    if (!file) {
        http.end();
        return false;
    }
    
    static uint8_t buffer[1024];
    int totalBytes = 0;
    int len = http.getSize();
    unsigned long startT = millis();
    
    while (http.connected() && (len > 0 || len == -1)) {
        if (millis() - startT > 10000) break;
        size_t size = s->available();
        if (size > 0) {
            int bytes = s->read(buffer, ((size > sizeof(buffer)) ? sizeof(buffer) : size));
            if (bytes > 0) {
                file.write(buffer, bytes);
                totalBytes += bytes;
                if (len > 0) len -= bytes;
            }
        } else {
            delay(1);
        }
    }
    file.close();
    http.end();
    
    serialPrintf("Photo saved: %s (%d bytes)\n", filename.c_str(), totalBytes);
    return true;
}
