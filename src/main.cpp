#include <Arduino.h>
#include <WiFi.h>
#include "Global.h"
#include "StorageManager.h"
#include "CameraClient.h"
#include "MjpegParser.h"
#include "UIManager.h"
#include "InputHandler.h"
#include "TimelapseManager.h"
#include "FilterManager.h"
#include "WiFiServerManager.h"

// 流控制
WiFiClient streamClient;
unsigned long lastDisplayTime = 0;
const unsigned long minFrameInterval = 10;
unsigned long fpsLastTime = 0;
int fpsFrameCount = 0;

// 相机参数变更后异步刷新 status.txt
static bool pendingStatusRefresh = false;
static unsigned long lastParamChangeTime = 0;
const unsigned long STATUS_REFRESH_DELAY = 500; // 0.5秒后刷新

void setup() {
    M5Cardputer.begin();
    Serial.begin(115200);
    
    UIManager::init();
    UIManager::displayLine("Initializing SD...", true);
    StorageManager::init();
    
    UIManager::displayLine("Connecting WiFi...");
    WiFi.begin("UnitCamS3-WiFi", "");
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 20) {
        delay(500);
        Serial.print(".");
        retry++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        UIManager::displayLine("WiFi connected!");
        UIManager::displayLine("IP: " + WiFi.localIP().toString());
        CameraClient::setResolution(CAMERA_RESOLUTION_LOW);
        CameraClient::setQuality(CAMERA_QUALITY_STREAM);
        appState.isRestartStream = true;
    } else {
        UIManager::displayLine("WiFi failed!");
    }
}

void loop() {
    M5Cardputer.update();
    
    if (isTimelapseMode) {
        TimelapseManager::update();
        return;
    }
    
    // Handle Input
    InputEvent ev = InputHandler::handle();
    switch (ev) {
        case EVENT_RESTART: ESP.restart(); break;
        case EVENT_TIMELAPSE: TimelapseManager::start(); break;
        case EVENT_STATUS: {
            String status;
            if (CameraClient::getStatus(status)) {
                StorageManager::saveCameraStatus(status);
                UIManager::showStatus(status);
                UIManager::clear();
                streamClient.stop();
                delay(50); // 稳定性缓冲
                appState.isRestartStream = true;
            }
            break;
        }
        case EVENT_HELP: {
            UIManager::showHelp();
            UIManager::clear();
            streamClient.stop();
            delay(50); // 稳定性缓冲
            appState.isRestartStream = true;
            break;
        }
        case EVENT_BRIGHTNESS_UP:   if (currentBrightness < 2)  { CameraClient::setParameter("brightness", ++currentBrightness); pendingStatusRefresh = true; lastParamChangeTime = millis(); } break;
        case EVENT_BRIGHTNESS_DOWN: if (currentBrightness > -2) { CameraClient::setParameter("brightness", --currentBrightness); pendingStatusRefresh = true; lastParamChangeTime = millis(); } break;
        case EVENT_CONTRAST_UP:     if (currentContrast < 2)    { CameraClient::setParameter("contrast",   ++currentContrast);   pendingStatusRefresh = true; lastParamChangeTime = millis(); } break;
        case EVENT_CONTRAST_DOWN:   if (currentContrast > -2)   { CameraClient::setParameter("contrast",   --currentContrast);   pendingStatusRefresh = true; lastParamChangeTime = millis(); } break;
        case EVENT_SATURATION_UP:   if (currentSaturation < 2)  { CameraClient::setParameter("saturation", ++currentSaturation); pendingStatusRefresh = true; lastParamChangeTime = millis(); } break;
        case EVENT_SATURATION_DOWN: if (currentSaturation > -2) { CameraClient::setParameter("saturation", --currentSaturation); pendingStatusRefresh = true; lastParamChangeTime = millis(); } break;
        case EVENT_SHARPNESS_UP:    if (currentSharpness < 2)   { CameraClient::setParameter("sharpness",  ++currentSharpness);  pendingStatusRefresh = true; lastParamChangeTime = millis(); } break;
        case EVENT_SHARPNESS_DOWN:  if (currentSharpness > -2)  { CameraClient::setParameter("sharpness",  --currentSharpness);  pendingStatusRefresh = true; lastParamChangeTime = millis(); } break;
        
        // 滤镜切换:同一按键再按则关闭滤镜
        case EVENT_FILTER_GAMEBOY: {
            FilterMode next = (FilterManager::getFilter() == FILTER_GAMEBOY) ? FILTER_NONE : FILTER_GAMEBOY;
            FilterManager::setFilter(next);
            snprintf(appState.overlayMsg, sizeof(appState.overlayMsg),
                     next == FILTER_NONE ? "Filter: OFF" : "Filter: GameBoy");
            appState.overlayTimestamp = millis();
            break;
        }
        case EVENT_FILTER_PIXELATE: {
            FilterMode next = (FilterManager::getFilter() == FILTER_PIXELATE) ? FILTER_NONE : FILTER_PIXELATE;
            FilterManager::setFilter(next);
            snprintf(appState.overlayMsg, sizeof(appState.overlayMsg),
                     next == FILTER_NONE ? "Filter: OFF" : "Filter: Pixelate");
            appState.overlayTimestamp = millis();
            break;
        }
        case EVENT_FILTER_GLITCH: {
            FilterMode next = (FilterManager::getFilter() == FILTER_GLITCH) ? FILTER_NONE : FILTER_GLITCH;
            FilterManager::setFilter(next);
            snprintf(appState.overlayMsg, sizeof(appState.overlayMsg),
                     next == FILTER_NONE ? "Filter: OFF" : "Filter: Glitch");
            appState.overlayTimestamp = millis();
            break;
        }
        case EVENT_WIFI_SERVER: {
            serialPrintf("Entering WiFi Server Mode...\n");
            // 1. 停止当前所有流与连接
            streamClient.stop();
            
            // 2. 准备 UI 
            UIManager::showWiFiPortal();
            delay(300); // 防抖，防止进入瞬间检测到按键未释放又退出了
            
            // 3. 开启服务器
            WiFiServerManager::begin();
            
            // 4. 阻塞循环，直到再次按下 W
            bool exitPortal = false;
            while (!exitPortal) {
                M5Cardputer.update();
                WiFiServerManager::loop();
                if (InputHandler::handle() == EVENT_WIFI_SERVER) {
                    exitPortal = true;
                }
                delay(10);
            }
            
            // 5. 关闭服务器并恢复
            serialPrintf("Exiting WiFi Mode...\n");
            WiFiServerManager::stop();
            UIManager::displayLine("WiFi Server Stopped", true);
            
            // 6. 重新初始化相机 WiFi
            UIManager::displayLine("Reconnecting to Camera...");
            WiFi.begin("UnitCamS3-WiFi", "");
            int retry = 0;
            while (WiFi.status() != WL_CONNECTED && retry < 20) {
                delay(500);
                retry++;
            }
            appState.isRestartStream = true;
            UIManager::clear();
            break;
        }
        default:
            if (ev >= EVENT_EFFECT_START && ev < EVENT_EFFECT_START + 7) {
                CameraClient::setSpecialEffect(ev - EVENT_EFFECT_START);
            }
            break;
    }
    
    // Handle BtnA (Capture)
    static unsigned long btnStartTime = 0;
    static bool isBtnPressed = false;
    static bool isBurst = false;
    static int burstCount = 0;
    static unsigned long lastBurstTime = 0;
    
    if (M5Cardputer.BtnA.isPressed()) {
        if (!isBtnPressed) {
            isBtnPressed = true;
            btnStartTime = millis();
            isBurst = false;
            burstCount = 0;
            streamClient.stop();
            CameraClient::setResolution(CAMERA_RESOLUTION_HIGH);
            delay(200);
        } else if (millis() - btnStartTime > 500) {
            if (millis() - lastBurstTime > 200) {
                lastBurstTime = millis();
                isBurst = true;
                char filename[64];
                sprintf(filename, "/images/BURST_%lu_%d.jpg", millis(), burstCount++);
                if (CameraClient::downloadPhoto(filename)) {
                    snprintf(appState.overlayMsg, sizeof(appState.overlayMsg), "Saved #%d", burstCount);
                    appState.overlayTimestamp = millis();
                    UIManager::renderStream();
                }
            }
        }
    } else if (isBtnPressed) {
        isBtnPressed = false;
        if (!isBurst) {
            CameraClient::triggerCapture();
            delay(100);
            char filename[64];
            sprintf(filename, "/images/IMG_%lu.jpg", millis());
            if (CameraClient::downloadPhoto(filename)) {
                snprintf(appState.overlayMsg, sizeof(appState.overlayMsg), "Saved: %lu", millis());
                appState.overlayTimestamp = millis();
                // 创意滤镜后期处理
                if (FilterManager::getFilter() != FILTER_NONE) {
                    UIManager::processAndSaveFilteredPhoto(filename);
                }
            }
        }
        CameraClient::setResolution(CAMERA_RESOLUTION_LOW);
        appState.isRestartStream = true;
    }
    
    // Handle Stream
    if (WiFi.status() == WL_CONNECTED) {
        if (!streamClient.connected() && appState.isRestartStream) {
            appState.isRestartStream = false;
            if (streamClient.connect("192.168.4.1", 80)) {
                MjpegParser::processStream(streamClient, true);
                streamClient.print("GET /api/v1/stream HTTP/1.1\r\nHost: 192.168.4.1\r\nConnection: keep-alive\r\n\r\n");
                streamClient.setNoDelay(true);
                
                // 强同步：等待 HTTP Header 结束
                unsigned long syncStart = millis();
                String syncBuf = "";
                while (millis() - syncStart < 2000) {
                    if (streamClient.available()) {
                        char sc = streamClient.read();
                        syncBuf += sc;
                        if (syncBuf.endsWith("\r\n\r\n") || syncBuf.endsWith("\n\n")) break;
                        if (syncBuf.length() > 200) syncBuf = syncBuf.substring(150);
                    }
                }
            }
        } else if (streamClient.connected()) {
            MjpegParser::processStream(streamClient);
            
            // 自愈逻辑：连续 15 帧错误则重连
            if (appState.consecutiveErrors >= 15) {
                serialPrintf("Consecutive errors %d, reconnecting...\n", appState.consecutiveErrors);
                streamClient.stop();
                appState.isRestartStream = true;
                appState.consecutiveErrors = 0;
            }
        }
    }
    
    // Display Logic
    if (appState.currentState == STATE_DISPLAYING && millis() - lastDisplayTime >= minFrameInterval) {
        lastDisplayTime = millis();
        if (UIManager::renderStream()) {
            fpsFrameCount++;
        }
        appState.currentState = STATE_RECEIVING;
    }
    
    // FPS Calc
    if (millis() - fpsLastTime >= 1000) {
        currentFps = fpsFrameCount;
        fpsFrameCount = 0;
        fpsLastTime = millis();
    }
    
    // 相机参数变更后延迟刷新状态到 /images/status.txt
    if (pendingStatusRefresh && isSDInitialized &&
        millis() - lastParamChangeTime >= STATUS_REFRESH_DELAY) {
        pendingStatusRefresh = false;
        String newStatus;
        if (CameraClient::getStatus(newStatus)) {
            // 写入 /images/status.txt（doc.txt 要求路径）
            File sf = SD.open("/images/status.txt", FILE_WRITE);
            if (sf) { sf.print(newStatus); sf.close(); }
            // 同步写主状态文件
            StorageManager::saveCameraStatus(newStatus);
            serialPrintf("[Status] Refreshed status.txt after param change\n");
        }
    }
}
