#ifndef CAMERA_CLIENT_H
#define CAMERA_CLIENT_H

#include "Global.h"
#include <WiFi.h>
#include <HTTPClient.h>

class CameraClient {
public:
    static bool setParameter(const String& param, int value);
    static bool setResolution(int resolution);
    static bool setQuality(int quality);
    static bool setSpecialEffect(int effect);
    static bool getStatus(String& response);
    static bool triggerCapture();
    static bool downloadPhoto(const String& filename, bool isBurst = false, int burstIndex = 0);
};

#endif // CAMERA_CLIENT_H
