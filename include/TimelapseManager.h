#ifndef TIMELAPSE_MANAGER_H
#define TIMELAPSE_MANAGER_H

#include "Global.h"

class TimelapseManager {
public:
    static bool start();
    static void stop();
    static void update();
    static bool isRunning() { return isTimelapseMode; }

private:
    static bool createSessionDir();
    static bool capture();
    
    static String currentDir;
    static int photoCount;
    static unsigned long lastShotTime;
    static unsigned long startTime;
    static unsigned long interval;
    static unsigned long lastActionTime;
    static bool isScreenOff;
};

#endif // TIMELAPSE_MANAGER_H
