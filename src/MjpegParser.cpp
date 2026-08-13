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
            {
                bool isEOI = (appState.lastByte == 0xFF && c == 0xD9);
                appState.lastByte = c; // 记录上一个字符用于 EOI 识别

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

                    // 识别到结束符 (EOI)
                    if (isEOI) {
                        if (appState.networkSize >= 2048 && appState.networkSize < GLOBAL_MAX_JPEG_SIZE) {
                            if (!appState.sizeCached) {
                                int w = 0, h = 0;
                                if (parseJpegSize(appState.networkBuffer, appState.networkSize, w, h)) {
                                    appState.cachedImgWidth = w;
                                    appState.cachedImgHeight = h;
                                    appState.cachedScale = 0.5f; 
                                    appState.sizeCached = true;
                                }
                            }
                            appState.currentState = STATE_DISPLAYING;
                        } else {
                            appState.networkSize = 0;
                        }
                        appState.parseState = AppState::P_BOUNDARY;
                    } 
                    else if (appState.frameReadCount >= GLOBAL_MAX_JPEG_SIZE) {
                        appState.networkSize = 0;
                        appState.parseState = AppState::P_BOUNDARY;
                    }
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

    // 网络 Socket 赶帧防积压：大动态转动时若积压深，迅速清理旧滞后包
    if (client.available() > 8192) {
        while (client.available() > 4096) {
            uint8_t dummy[512];
            client.read(dummy, sizeof(dummy));
        }
    }

    while (true) {
        if (cachePos >= cacheLen) {
            if (client.available() == 0) break;
            cachePos = 0;
            cacheLen = client.read(readCache, sizeof(readCache));
            if (cacheLen <= 0) break;
        }

        // 块加速传输：当处于 JPEG 数据体且有大量 cache 剩余时，进行极速 block memcpy
        if (appState.parseState == AppState::P_JPEG_DATA && cState == ChunkState::CS_DATA && appState.networkSize >= 2) {
            int remInCache = cacheLen - cachePos;
            if (remInCache > 2) {
                uint8_t* p = &readCache[cachePos];
                int copyN = remInCache;
                bool foundEOI = false;
                
                // 限制不超过 current chunk 剩余长度
                if (chunkSize > 0 && (uint32_t)copyN > chunkSize) {
                    copyN = chunkSize;
                }
                
                // 搜索是否包含 EOI (0xFF 0xD9)
                for (int i = 0; i < copyN - 1; i++) {
                    if (p[i] == 0xFF && p[i + 1] == 0xD9) {
                        copyN = i + 2;
                        foundEOI = true;
                        break;
                    }
                }
                
                if (appState.networkSize + copyN > GLOBAL_MAX_JPEG_SIZE) {
                    copyN = GLOBAL_MAX_JPEG_SIZE - appState.networkSize;
                }
                
                if (copyN > 0) {
                    memcpy(&appState.networkBuffer[appState.networkSize], p, copyN);
                    appState.networkSize += copyN;
                    appState.frameReadCount += copyN;
                    appState.lastByte = p[copyN - 1];
                    cachePos += copyN;
                    if (chunkSize >= (uint32_t)copyN) chunkSize -= copyN;
                    else chunkSize = 0;
                    
                    if (chunkSize == 0) cState = ChunkState::CS_TRAILER;
                    
                    if (foundEOI) {
                        if (appState.networkSize >= 2048) {
                            appState.currentState = STATE_DISPLAYING;
                        } else {
                            appState.networkSize = 0;
                        }
                        appState.parseState = AppState::P_BOUNDARY;
                        return;
                    }
                    continue;
                }
            }
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
