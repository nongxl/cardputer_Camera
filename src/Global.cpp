#include "Global.h"
#include <stdarg.h>

AppState appState;
bool isSDInitialized = false;
bool isTimelapseMode = false;
int currentBrightness = 0;
int currentContrast = 0;
int currentSaturation = 0;
int currentSharpness = 0;
float currentFps = 0;

void serialPrintf(const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Serial.print(buffer);
}
