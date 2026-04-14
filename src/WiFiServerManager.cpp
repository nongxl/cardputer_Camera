#include "WiFiServerManager.h"
#include <WiFi.h>
#include <FS.h>
#include <functional>

WebServer* WiFiServerManager::_server = nullptr;
bool WiFiServerManager::_isRunning = false;

void WiFiServerManager::begin() {
    if (_isRunning) return;

    // 1. 切换 WiFi 模式
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Cardputer-Cam", "");
    
    serialPrintf("[WiFiServer] AP Started: Cardputer-Cam\n");
    serialPrintf("[WiFiServer] IP: %s\n", WiFi.softAPIP().toString().c_str());

    // 2. 初始化服务器
    _server = new WebServer(80);
    _server->on("/", handleRoot);
    _server->on("/download", handleDownload);
    _server->onNotFound(handleNotFound);
    _server->begin();
    
    _isRunning = true;
}

void WiFiServerManager::stop() {
    if (!_isRunning) return;
    
    if (_server) {
        _server->stop();
        delete _server;
        _server = nullptr;
    }
    
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    
    _isRunning = false;
    serialPrintf("[WiFiServer] Stopped\n");
}

void WiFiServerManager::loop() {
    if (_isRunning && _server) {
        _server->handleClient();
    }
}

void WiFiServerManager::handleRoot() {
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<title>M5Cardputer Camera Gallery</title>";
    html += "<style>";
    html += "body { background-color: #1a1a1a; color: #e0e0e0; font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; margin: 0; padding: 20px; }";
    html += "h1 { color: #00e5ff; text-align: center; border-bottom: 2px solid #333; padding-bottom: 10px; }";
    html += ".container { max-width: 1000px; margin: 0 auto; }";
    html += ".gallery { display: grid; grid-template-columns: repeat(auto-fill, minmax(160px, 1fr)); gap: 15px; padding: 0; list-style: none; }";
    html += ".file-card { background: #2d2d2d; border-radius: 12px; overflow: hidden; transition: 0.3s; box-shadow: 0 4px 6px rgba(0,0,0,0.3); position: relative; }";
    html += ".file-card.timelapse { border: 1px solid #00e5ff; box-shadow: 0 0 10px rgba(0,229,255,0.2); }";
    html += ".file-card:hover { transform: translateY(-5px); box-shadow: 0 8px 15px rgba(0,0,0,0.5); }";
    html += ".preview { width: 100%; height: 120px; object-fit: cover; background: #333; display: block; border-bottom: 1px solid #444; }";
    html += ".info { padding: 10px; text-align: center; }";
    html += ".file-name { font-size: 0.75em; display: block; margin-bottom: 8px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; color: #888; }";
    html += ".tag { position: absolute; top: 5px; right: 5px; background: #00e5ff; color: #000; font-size: 0.6em; padding: 2px 6px; border-radius: 4px; font-weight: bold; }";
    html += ".download-btn { background: #00e5ff; color: #000; text-decoration: none; padding: 5px 12px; border-radius: 4px; font-weight: bold; font-size: 0.8em; display: inline-block; }";
    html += ".footer { text-align: center; margin-top: 40px; font-size: 0.8em; color: #666; padding-bottom: 20px; }";
    html += "</style></head><body><div class='container'>";
    html += "<h1>Cardputer Gallery</h1>";
    html += "<ul class='gallery'>";
 
    std::function<int(const char*, bool)> scanDir = [&](const char* path, bool parentIsTimelapse) {
        if (!SD.exists(path)) return 0;
        File dir = SD.open(path);
        if (!dir || !dir.isDirectory()) return 0;
        
        File file = dir.openNextFile();
        int count = 0;
        while (file) {
            String fullFileName = String(file.name());
            String fileNameOnly = fullFileName.substring(fullFileName.lastIndexOf('/') + 1);
            
            if (file.isDirectory()) {
                // 如果是二级目录，仅扫描一次（不深层递归，防止死循环或过深）
                // 约定：/images/TL_... 类型被视为 Timelapse
                if (String(path) == "/images") {
                    bool isTL = fileNameOnly.startsWith("TL_");
                    count += scanDir(fullFileName.c_str(), isTL);
                }
            } else if (fullFileName.endsWith(".jpg") || fullFileName.endsWith(".bmp")) {
                // 自动提取相对于 /images/ 的路径
                String relPath = fullFileName;
                if (relPath.startsWith("/images/")) {
                    relPath = relPath.substring(8); 
                } else if (relPath.startsWith("images/")) {
                    relPath = relPath.substring(7);
                }
                
                html += parentIsTimelapse ? "<li class='file-card timelapse'>" : "<li class='file-card'>";
                if (parentIsTimelapse) html += "<span class='tag'>TIMELAPSE</span>";
                html += "<a href='/download?file=" + relPath + "' target='_blank'>";
                html += "<img class='preview' src='/download?file=" + relPath + "' loading='lazy'>";
                html += "</a>";
                html += "<div class='info'>";
                html += "<span class='file-name'>" + fileNameOnly + "</span>";
                html += "<a class='download-btn' href='/download?file=" + relPath + "' download>Save</a>";
                html += "</div>";
                html += "</li>";
                count++;
            }
            file = dir.openNextFile();
        }
        return count;
    };

    int total = scanDir("/images", false);

    if (total == 0) html += "<li>No JPG/BMP images found.</li>";

    html += "</ul>";
    html += "<div class='footer'>Powered by M5Cardputer & ESP32-S3</div>";
    html += "</div></body></html>";
    
    _server->send(200, "text/html", html);
}

void WiFiServerManager::handleDownload() {
    if (!_server->hasArg("file")) {
        _server->send(400, "text/plain", "Missing file parameter");
        return;
    }
    
    String fileName = _server->arg("file");
    String fullPath = "/images/" + fileName;
    
    if (!SD.exists(fullPath)) {
        _server->send(404, "text/plain", "File not found");
        return;
    }
    
    File file = SD.open(fullPath);
    String contentType = "application/octet-stream";
    if (fileName.endsWith(".jpg")) contentType = "image/jpeg";
    if (fileName.endsWith(".bmp")) contentType = "image/bmp";
    
    _server->streamFile(file, contentType);
    file.close();
}

void WiFiServerManager::handleNotFound() {
    _server->send(404, "text/plain", "Not Found");
}
