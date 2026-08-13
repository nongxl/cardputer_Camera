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

// M5 丝印贴纸缤纷色彩定义 (RGB565 16-bit)
#define M5_COLOR_ORANGE   0xFA60  // M5 标志亮橙 (#FF6A00)
#define M5_COLOR_YELLOW   0xFFE0  // 贴纸亮黄 (#FFE600)
#define M5_COLOR_CYAN     0x07FF  // 电光青 (#00E5FF)
#define M5_COLOR_PINK     0xF81F  // 霓虹粉紫 (#FF2D55)
#define M5_COLOR_GREEN    0x07E0  // 荧光绿 (#00FF66)
#define M5_COLOR_DARK_BG  0x10A2  // 深青灰底色 (#121620)
#define M5_COLOR_CARD_BG  0x2128  // 深卡片背景色 (#202636)
#define M5_COLOR_WHITE    0xFFFF  // 纯白
#define M5_COLOR_BLACK    0x0000  // 纯黑

// 统一 UI 语义 Design Tokens
#define UI_COLOR_BG         M5_COLOR_DARK_BG  // 全屏主背景 (0x10A2)
#define UI_COLOR_SURFACE    M5_COLOR_CARD_BG  // 卡片/交替行背景 (0x2128)
#define UI_COLOR_HEADER_BG  M5_COLOR_ORANGE   // 标头 Header 背景 (亮橙)
#define UI_COLOR_HEADER_TXT M5_COLOR_BLACK    // 标头 Header 文字 (纯黑)
#define UI_COLOR_SELECT_BG  M5_COLOR_YELLOW   // 选中项/最新日志背景 (亮黄)
#define UI_COLOR_SELECT_TXT M5_COLOR_BLACK    // 选中项/最新日志文字 (纯黑)
#define UI_COLOR_BORDER     M5_COLOR_CYAN     // 弹窗边框/分割线/准星 (电光青)
#define UI_COLOR_TEXT_MAIN  M5_COLOR_WHITE    // 普通正文说明文字 (纯白)
#define UI_COLOR_METRIC     M5_COLOR_GREEN    // 监控读数/进度条 (荧光绿)
#define UI_COLOR_ALERT      M5_COLOR_PINK     // 捕获提示/异常 Overlays (霓虹粉)

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
  float cachedScale;
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
  uint8_t lastByte;
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

// FilterManager 在 Global.h 之后 include（避免循环依赖，通过前置声明）
// currentFilter 定义在 FilterManager.cpp，此处作为全局变量转发
// 注：FilterMode 类型定义在 FilterManager.h，使用者需单独 include FilterManager.h

#endif // GLOBAL_H
