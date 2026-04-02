#ifndef GLOBAL_H
#define GLOBAL_H

#include <Arduino.h>
#include <M5Cardputer.h>

// 全局常量定义
#define CAMERA_RESOLUTION_LOW 6     // UnitCamS3-5MP: 6 = 320x240
#define CAMERA_RESOLUTION_HIGH 10   // 高分辨率 640x480
#define CAMERA_RESOLUTION_TIMELAPSE 10 // timelapse模式分辨率

#define CAMERA_QUALITY_STREAM    2  // 串流模式画质
#define CAMERA_QUALITY_TIMELAPSE 0  // timelapse拍摄画质

#define GLOBAL_MAX_JPEG_SIZE 64 * 1024 // 64KB
#define MIN_JPEG_SIZE 2000            // 2KB

// 状态机定义
enum StreamState {
  STATE_RECEIVING,
  STATE_DISPLAYING
};

// 应用状态结构体
typedef struct {
  uint8_t networkBuffer[GLOBAL_MAX_JPEG_SIZE];
  size_t networkSize;
  StreamState currentState;
  bool isRestartStream;
  bool isCaptureReq;
  bool sizeCached;
  int cachedImgWidth;
  int cachedImgHeight;
  int cachedDrawX;
  int cachedDrawY;
  
  enum MjpegParseState { P_HTTP_HEADERS, P_BOUNDARY, P_FRAME_HEADERS, P_JPEG_DATA };
  MjpegParseState parseState;
  String lineBuffer;
  int expectedCL;
  int frameReadCount;
  bool boundaryFound;
  String boundary;
  int consecutiveErrors;
  int lastValidSize;
  char overlayMsg[64];
  uint32_t overlayTimestamp;
} AppState;

// 外部引用全局变量
extern AppState appState;
extern bool isSDInitialized;
extern bool isTimelapseMode;
extern int currentBrightness;
extern int currentContrast;
extern int currentSaturation;
extern int currentSharpness;
extern float currentFps;

// 辅助工具
void serialPrintf(const char* format, ...);

#endif // GLOBAL_H
