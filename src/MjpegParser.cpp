#include "MjpegParser.h"

void MjpegParser::handleByte(uint8_t c) {
    if (appState.currentState == STATE_DISPLAYING) return;
    
    switch (appState.parseState) {
        case AppState::P_HTTP_HEADERS:
            if (c == '\n') {
                String header = appState.lineBuffer;
                header.toLowerCase();
                if (header.length() == 0) appState.parseState = AppState::P_BOUNDARY;
                else if (header.startsWith("content-type:") && header.indexOf("boundary=") > 0) {
                    appState.boundary = appState.lineBuffer.substring(header.indexOf("boundary=") + 9);
                }
                appState.lineBuffer = "";
            } else if (c != '\r') appState.lineBuffer += (char)c;
            break;

        case AppState::P_BOUNDARY:
            if (c == '\n') {
                if (appState.lineBuffer.startsWith("--")) appState.parseState = AppState::P_FRAME_HEADERS;
                appState.lineBuffer = "";
            } else if (c != '\r') appState.lineBuffer += (char)c;
            break;

        case AppState::P_FRAME_HEADERS:
            if (c == '\n') {
                String header = appState.lineBuffer;
                header.toLowerCase();
                if (header.length() == 0) {
                    appState.parseState = AppState::P_JPEG_DATA; 
                    appState.networkSize = 0; 
                    appState.frameReadCount = 0;
                } else if (header.startsWith("content-length:")) {
                    appState.expectedCL = header.substring(15).toInt();
                }
                appState.lineBuffer = "";
            } else if (c != '\r') appState.lineBuffer += (char)c;
            break;

        case AppState::P_JPEG_DATA:
            if (appState.networkSize == 0) {
                if (c == 0xFF) appState.networkBuffer[appState.networkSize++] = c;
            } else if (appState.networkSize == 1) {
                if (c == 0xD8) appState.networkBuffer[appState.networkSize++] = c;
                else if (c != 0xFF) appState.networkSize = 0;
            } else {
                if (appState.networkSize < GLOBAL_MAX_JPEG_SIZE) {
                    appState.networkBuffer[appState.networkSize++] = c;
                }
                appState.frameReadCount++;

                if (appState.networkSize >= 2 && 
                    appState.networkBuffer[appState.networkSize-2] == 0xFF && 
                    appState.networkBuffer[appState.networkSize-1] == 0xD9) {
                    
                    if (appState.networkSize >= 2048) {
                        // 性能优化：仅在缓存失效时解析分辨率并计算居中偏移
                        if (!appState.sizeCached) {
                            int w = 0, h = 0;
                            if (parseJpegSize(appState.networkBuffer, appState.networkSize, w, h)) {
                                appState.cachedImgWidth = w;
                                appState.cachedImgHeight = h;
                                // 性能与内存优化：1/2 快速解码已在 UIManager 中配合 1.5x 硬件缩放工作
                                appState.cachedScale = 0.5f; 
                                appState.sizeCached = true;
                            }
                        }
                        appState.currentState = STATE_DISPLAYING;
                    } else {
                        appState.networkSize = 0;
                    }
                    appState.parseState = AppState::P_BOUNDARY;
                } else if (appState.frameReadCount >= 30000) { // 提高熔断阈值至 30KB
                    appState.networkSize = 0;
                    appState.parseState = AppState::P_BOUNDARY;
                }
            }
            break;
    }
}

void MjpegParser::processStream(WiFiClient& client, bool forceReset) {
    enum ChunkState { CS_SIZE, CS_DATA, CS_TRAILER };
    static ChunkState cState = CS_SIZE;
    static uint32_t chunkSize = 0;
    static String sizeBuf = "";
    
    static uint8_t readCache[1024];
    static int cachePos = 0;
    static int cacheLen = 0;

    if (forceReset) {
        cState = CS_SIZE; chunkSize = 0; sizeBuf = ""; cachePos = 0; cacheLen = 0;
        appState.parseState = AppState::P_HTTP_HEADERS;
        appState.networkSize = 0;
        appState.currentState = STATE_RECEIVING;
        return;
    }

    if (appState.currentState == STATE_DISPLAYING) return;

    while (true) {
        if (cachePos >= cacheLen) {
            if (client.available() == 0) break;
            cachePos = 0;
            cacheLen = client.read(readCache, sizeof(readCache));
            if (cacheLen <= 0) break;
        }

        uint8_t c = readCache[cachePos++];
        switch (cState) {
            case ChunkState::CS_SIZE:
                if (c == '\n') {
                    if (sizeBuf.length() > 0) {
                        chunkSize = strtol(sizeBuf.c_str(), NULL, 16);
                        if (chunkSize == 0) { client.stop(); return; }
                        cState = ChunkState::CS_DATA;
                    }
                    sizeBuf = "";
                } else if (isxdigit(c)) sizeBuf += (char)c;
                break;

            case ChunkState::CS_DATA:
                handleByte(c);
                if (chunkSize > 0) chunkSize--;
                if (chunkSize == 0) cState = ChunkState::CS_TRAILER;
                if (appState.currentState == STATE_DISPLAYING) return;
                break;

            case ChunkState::CS_TRAILER:
                if (c == '\n') cState = ChunkState::CS_SIZE;
                break;
        }
    }
}

bool MjpegParser::parseJpegSize(uint8_t* jpegData, size_t jpegSize, int& width, int& height) {
    if (jpegSize < 10) return false;
    int index = 0;
    while (index < jpegSize - 4) {
        if (jpegData[index] == 0xFF && jpegData[index + 1] == 0xC0) {
            if (index + 7 < jpegSize) {
                height = (jpegData[index + 5] << 8) | jpegData[index + 6];
                width = (jpegData[index + 7] << 8) | jpegData[index + 8];
                return true;
            }
        }
        index++;
    }
    return false;
}
