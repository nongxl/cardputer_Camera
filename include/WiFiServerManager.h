#ifndef WIFI_SERVER_MANAGER_H
#define WIFI_SERVER_MANAGER_H

#include <WebServer.h>
#include <SD.h>
#include "Global.h"

class WiFiServerManager {
public:
    static void begin();
    static void stop();
    static void loop();
    static bool isRunning() { return _isRunning; }

private:
    static void handleRoot();
    static void handleDownload();
    static void handleNotFound();
    
    static WebServer* _server;
    static bool _isRunning;
};

#endif // WIFI_SERVER_MANAGER_H
