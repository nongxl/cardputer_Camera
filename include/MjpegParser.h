#ifndef MJPEG_PARSER_H
#define MJPEG_PARSER_H

#include "Global.h"
#include <WiFi.h>

class MjpegParser {
public:
    static void processStream(WiFiClient& client, bool forceReset = false);
    static bool parseJpegSize(uint8_t* jpegData, size_t jpegSize, int& width, int& height);
private:
    static void handleByte(uint8_t c);
};

#endif // MJPEG_PARSER_H
