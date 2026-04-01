#include <Arduino.h>
#include <M5Cardputer.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SD.h>
#include <SPI.h>
#include <ArduinoJson.h>

// 全局常量定义
#define CAMERA_RESOLUTION_LOW 6     // UnitCamS3-5MP: 6 = 320x240
#define CAMERA_RESOLUTION_HIGH 10   // 高分辨率 640x480
#define CAMERA_RESOLUTION_TIMELAPSE 10 // timelapse模式分辨率

#define CAMERA_QUALITY_STREAM    2  // 串流模式画质（相机支持的最低值=最高画质）
#define CAMERA_QUALITY_TIMELAPSE 0  // timelapse拍摄画质（与串流保持一致）

#define GLOBAL_MAX_JPEG_SIZE 64 * 1024 // 64KB (quality=2时约7KB，预留充足余量)
#define MIN_JPEG_SIZE 2000            // 最小有效 JPEG 帧大小 (2KB)，过滤噪声同时兼顾快速响应




// 全局变量
bool isSDInitialized = false;
bool isTimelapseMode = false;
int timelapsePhotoCount = 0;
unsigned long timelapseLastShotTime = 0;
unsigned long timelapseStartTime = 0;
unsigned long timelapseInterval = 5000; // 5秒间隔
unsigned long lastUserActionTime = 0;
const unsigned long screenOffTimeout = 60000; // 1分钟
bool isScreenOff = false;
String currentTimelapseDir = "";

// 按键防抖
unsigned long lastKeyPressTime = 0;
const unsigned long keyDebounceDelay = 500; // 500ms

// 相机参数
int currentBrightness = 0;
int currentContrast = 0;
int currentSaturation = 0;
int currentSharpness = 0;

// 帧率统计与显示控制相关
unsigned long lastDisplayTime = 0;
const unsigned long minFrameInterval = 10; // 10ms (100 FPS limit)
float currentFps = 0;
int fpsFrameCount = 0;
unsigned long fpsLastTime = 0;


// 流客户端
WiFiClient streamClient;
HTTPClient streamHttp;

// 离屏渲染画布 (用于解决 Overlay 闪烁)
M5Canvas canvas(&M5Cardputer.Display);


// Overlay display timing
unsigned long lastOverlayUpdate = 0; // 上一次覆盖信息更新时间
const unsigned long overlayInterval = 1000; // 1秒更新一次


// 状态机定义
enum StreamState {
  STATE_RECEIVING,
  STATE_DISPLAYING
};

// 应用状态结构体
typedef struct {
  // --- 缓冲区 ---
  // 用于接收 MJPEG 流的 buffer
  uint8_t networkBuffer[GLOBAL_MAX_JPEG_SIZE];
  size_t networkSize;
  
  // 移除 displayBuffer，直接在显示时锁定 networkBuffer 提高内存效率
  
  // 网络接收与显示的状态机
  StreamState currentState;

  
  // 流状态
  bool isRestartStream;
  bool isCaptureReq;
  
  // 图像尺寸缓存
  bool sizeCached;
  int cachedImgWidth;
  int cachedImgHeight;
  
  // MJPEG 解析状态
  enum MjpegParseState { P_HTTP_HEADERS, P_BOUNDARY, P_FRAME_HEADERS, P_JPEG_DATA };
  MjpegParseState parseState;
  String lineBuffer;
  int expectedCL;
  int frameReadCount;
  
  // 网络状态
  bool boundaryFound;
  String boundary;
  int consecutiveErrors;     // 连续解码错误计数
  int lastValidSize;         // 上一次成功解码的图像大小
  char overlayMsg[64];       // 屏幕底部浮动提示信息
  uint32_t overlayTimestamp; // 浮动提示显示开始时间
} AppState;






// 全局应用状态
AppState appState;

// 初始化应用状态
void initAppState() {
  // 初始化缓冲区和状态机
  appState.networkSize = 0;
  appState.currentState = STATE_RECEIVING;

  
  // 初始化流状态
  appState.isRestartStream = false;
  appState.isCaptureReq = false;
  
  // 初始化图像尺寸缓存
  appState.sizeCached = false;
  appState.cachedImgWidth = 0;
  appState.cachedImgHeight = 0;
  
  // 初始化网络状态
  appState.boundaryFound = false;
  appState.boundary = "";
  
  // 初始化 MJPEG 解析器状态
  appState.parseState = AppState::P_HTTP_HEADERS;
  appState.lineBuffer = "";
  appState.expectedCL = 0;
  appState.frameReadCount = 0;
  appState.consecutiveErrors = 0;
  appState.consecutiveErrors = 0;
  appState.lastValidSize = 0;

  appState.overlayMsg[0] = '\0';
  appState.overlayTimestamp = 0;
}





// 从WiFiClient读取一行
bool readLine(WiFiClient& client, String& line) {
  line = "";
  unsigned long startTime = millis();
  const unsigned long timeout = 1000; // 1秒超时
  
  while (client.available() == 0) {
    if (millis() - startTime > timeout) {
      return false;
    }
    delay(1);
  }
  
  while (client.available()) {
    char c = client.read();
    if (c == '\n') {
      break;
    }
    if (c != '\r') {
      line += c;
    }
  }
  
  return true;
}

// 解析HTTP头，提取Content-Length
bool parseHttpHeaders(WiFiClient& client, size_t& contentLength) {
  String line;
  contentLength = 0;
  bool headersComplete = false;
  
  while (!headersComplete) {
    if (!readLine(client, line)) {
      return false;
    }
    
    // 检查是否到达空行（头结束）
    if (line.length() == 0) {
      headersComplete = true;
    } 
    // 解析Content-Length
    else if (line.startsWith("Content-Length: ")) {
      contentLength = line.substring(16).toInt();
    }
  }
  
  return true;
}

// 解析JPEG尺寸
bool parseJpegSize(uint8_t* jpegData, size_t jpegSize, int& width, int& height) {
  if (jpegSize < 10) {
    return false;
  }
  
  int index = 0;
  while (index < jpegSize - 4) {
    if (jpegData[index] == 0xFF && jpegData[index + 1] == 0xC0) {
      // 找到SOF0标记
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

// 拍摄快照
bool captureSnapshot() {
  HTTPClient http;
  const char* captureUrl = "http://192.168.4.1/api/v1/capture";
  
  http.begin(captureUrl);
  http.addHeader("User-Agent", "M5Cardputer");
  http.setTimeout(15000);
  
  int code = http.GET();
  if (code != 200) {
    http.end();
    return false;
  }
  
  http.end();
  return true;
}

// 在屏幕上显示一行文本
void displayLine(String line, bool reset = false) {
  static int currentLine = 0;
  if (reset) {
    currentLine = 0;
    M5Cardputer.Display.fillScreen(BLACK);
  }
  M5Cardputer.Display.setCursor(10, 10 + currentLine * 12);
  M5Cardputer.Display.println(line);
  currentLine++;
  if (currentLine > 15) {
    currentLine = 0;
    M5Cardputer.Display.fillScreen(BLACK);
  }
}

// 串口打印（带错误处理）
void serialPrintf(const char* format, ...) {
  char buffer[256];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  Serial.print(buffer);
}

// 记录HTTP响应头
void logHttpResponseHeaders(String prefix, int code, HTTPClient& http) {
  if (code != 200) {
    serialPrintf("[%s] HTTP %d\n", prefix.c_str(), code);
  }
}

// 设置相机参数
bool setCameraParameter(String param, int value) {
  HTTPClient http;
  String url = String("http://192.168.4.1/api/v1/control?var=") + param + String("&val=") + value;
  
  http.begin(url);
  http.addHeader("User-Agent", "M5Cardputer");
  
  int code = http.GET();
  logHttpResponseHeaders(param.c_str(), code, http);
  
  if (code != 200) {
    http.end();
    return false;
  }
  
  http.end();
  return true;
}

// 设置相机分辨率
bool setCameraResolution(int resolution) {
  // 在屏幕上显示相机初始化信息
  displayLine(String("Setting res to ") + resolution + "...");
  Serial.printf("Setting camera resolution to %d...\n", resolution);

  
  HTTPClient http;
  String url = String("http://192.168.4.1/api/v1/control?var=framesize&val=") + resolution;
  
  http.begin(url);
  // 使用与Python代码一致的最小化请求头
  http.addHeader("User-Agent", "M5Cardputer");
  
  int code = http.GET();
  logHttpResponseHeaders("res", code, http);
  
  M5Cardputer.Display.setCursor(10, 115);
  
  if (code != 200) {
    serialPrintf("[Res] HTTP %d\n", code);
    // logLine(String("[Res] HTTP ") + code);
    M5Cardputer.Display.println("Resolution setup failed!");
    http.end();
    return false;
  }
  
  http.end();
  serialPrintf("Camera resolution set to %d successfully\n", resolution);
  // displayLine("Res set success");

  
  // 清除图像尺寸缓存（因为分辨率改变了）
  appState.sizeCached = false;
  appState.cachedImgWidth = 0;
  appState.cachedImgHeight = 0;
  
  return true;
}

// 设置相机质量
bool setCameraQuality(int quality) {
  // 在屏幕上显示相机质量设置信息
  displayLine(String("Setting quality to ") + quality + "...");
  Serial.printf("Setting camera quality to %d...\n", quality);
  
  HTTPClient http;
  String url = String("http://192.168.4.1/api/v1/control?var=quality&val=") + quality;
  
  http.begin(url);
  // 使用与Python代码一致的最小化请求头
  http.addHeader("User-Agent", "M5Cardputer");
  
  int code = http.GET();
  logHttpResponseHeaders("qual", code, http);
  
  if (code != 200) {
    serialPrintf("[Qual] HTTP %d\n", code);
    // logLine(String("[Qual] HTTP ") + code);
    displayLine("Quality setup failed!");
    http.end();
    return false;
  }
  
  http.end();
  serialPrintf("Camera quality set to %d successfully\n", quality);
  // logLine("Camera quality set successfully");
  displayLine("Camera quality set!");
  return true;
}

// 设置相机特效
bool setCameraSpecialEffect(int effect) {
  // 在屏幕上显示相机特效设置信息
  displayLine(String("Setting effect to ") + effect + "...");
  Serial.printf("Setting camera effect to %d...\n", effect);
  
  HTTPClient http;
  String url = String("http://192.168.4.1/api/v1/control?var=special_effect&val=") + effect;
  
  http.begin(url);
  // 使用与Python代码一致的最小化请求头
  http.addHeader("User-Agent", "M5Cardputer");
  
  int code = http.GET();
  logHttpResponseHeaders("effect", code, http);
  
  if (code != 200) {
    serialPrintf("[Effect] HTTP %d\n", code);
    // logLine(String("[Effect] HTTP ") + code);
    displayLine("Effect setup failed!");
    http.end();
    return false;
  }
  
  http.end();
  serialPrintf("Camera effect set to %d successfully\n", effect);
  // logLine("Camera effect set successfully");
  displayLine("Camera effect set!");
  return true;
}

// 获取相机配置并保存到SD卡
bool getCameraConfig() {
  // 在屏幕上显示获取配置信息
  displayLine("Getting camera status...");
  Serial.println("Getting camera status...");
  
  HTTPClient http;
  String url = "http://192.168.4.1/api/v1/status";
  
  http.begin(url);
  http.addHeader("User-Agent", "M5Cardputer");
  http.setTimeout(15000);
  
  int code = http.GET();
  logHttpResponseHeaders("status", code, http);
  
  if (code != 200) {
    serialPrintf("[Config] HTTP %d\n", code);
    displayLine("Failed to get status!");
    http.end();
    return false;
  }
  
  String response = http.getString();
  http.end();
  
  serialPrintf("Raw status data length: %d\n", response.length());
  serialPrintf("Raw status data from camera: %s\n", response.c_str());
  
  // 保存配置到SD卡
  if (isSDInitialized) {
    File file = SD.open("/camera_status.json", FILE_WRITE);
    if (file) {
      file.print(response);
      file.close();
    }
  }
  
  return true;
}

// 从SD卡加载相机状态
void loadCameraStatus() {
  if (!isSDInitialized) return;
  
  File file = SD.open("/camera_status.json");
  if (!file) return;
  
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, file);
  if (error) {
    file.close();
    return;
  }
  
  // 加载相机参数
  currentBrightness = doc["brightness"];
  currentContrast = doc["contrast"];
  currentSaturation = doc["saturation"];
  currentSharpness = doc["sharpness"];
  
  file.close();
}

// 创建timelapse目录
bool createTimelapseDir() {
  if (!isSDInitialized) return false;
  
  // 创建images目录
  if (!SD.exists("/images")) {
    if (!SD.mkdir("/images")) {
      return false;
    }
  }
  
  // 创建timelapse目录
  time_t now = time(nullptr);
  struct tm *timeinfo = localtime(&now);
  char dirname[40];
  sprintf(dirname, "/images/timelapse_%04d%02d%02d_%02d%02d%02d", 
          timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday,
          timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
  
  if (!SD.exists(dirname)) {
    if (!SD.mkdir(dirname)) {
      return false;
    }
  }
  
  currentTimelapseDir = dirname;
  return true;
}

// 拍摄timelapse照片
bool captureTimelapsePhoto() {
  serialPrintf("Capturing timelapse photo %d...\n", timelapsePhotoCount + 1);
  
  const char* captureUrl = "http://192.168.4.1/api/v1/capture";
  
  // 拍摄照片
  {
    HTTPClient http;
    http.begin(captureUrl);
    http.addHeader("User-Agent", "M5Cardputer");
    http.setTimeout(15000);
    //serialPrintf("[Timelapse] Step 1: Triggering capture...\n");
    int code = http.GET();
    http.end();
  }
  
  // 等待相机处理新图像
  delay(1000);
  
  // 获取刚刚拍摄的照片 (使用独立对象)
  HTTPClient http;
  http.begin(captureUrl);
  http.addHeader("User-Agent", "M5Cardputer");
  http.setTimeout(15000);
  //serialPrintf("[Timelapse] Step 2: Fetching photo...\n");
  int code = http.GET();
  
  if (code != 200) {
    serialPrintf("[Timelapse] HTTP Fetch Error: %d\n", code);
    http.end();


    // 即使拍摄失败，也要重置倒计时，避免卡在0秒
    timelapseLastShotTime = millis();
    return false;
  }
  
  String ct = http.header("Content-Type");
  //serialPrintf("[Timelapse] CT: %s\n", ct.c_str());
  
  // 验证内容类型是否为JPEG，但允许空内容类型（相机API可能不设置它）
  if (!ct.isEmpty() && !ct.startsWith("image/jpeg")) {
    //serialPrintf("[Timelapse] Unexpected content-type: %s\n", ct.c_str());
    http.end();
    timelapseLastShotTime = millis();
    return false;
  }
  
  // 读取JPEG数据
  WiFiClient* s = http.getStreamPtr();
  s->setNoDelay(true);
  s->setTimeout(10000);
  int len = http.getSize();
  
  //serialPrintf("[Timelapse] Content length: %d\n", len);
  
  // 寻找当前会话中最大的照片编号
  int maxPhotoNum = -1;
  File sessionDir = SD.open(currentTimelapseDir);
  if (sessionDir) {
    File file;
    while (file = sessionDir.openNextFile()) {
      String filename = file.name();
      if (filename.startsWith("IMG_")) {
        int num = filename.substring(4, 8).toInt();
        if (num > maxPhotoNum) {
          maxPhotoNum = num;
        }
      }
      file.close();
    }
    sessionDir.close();
  }
  
  // 创建带序号的文件名 (增加缓冲区大小以防止溢出导致 0x00000004 崩溃)
  char filename[128];
  snprintf(filename, sizeof(filename), "%s/IMG_%04d.jpg", currentTimelapseDir.c_str(), maxPhotoNum + 1);
  
  // 保存到SD卡
  File file = SD.open(filename, FILE_WRITE);
  if (!file) {
    http.end();
    timelapseLastShotTime = millis();
    return false;
  }

  
  // 读取并写入数据
  static uint8_t buffer[1024];
  int totalBytes = 0;
  while (http.connected() && (len > 0 || len == -1)) {
    size_t size = s->available();
    if (size > 0) {
      int bytes = s->read(buffer, ((size > sizeof(buffer)) ? sizeof(buffer) : size));
      if (bytes > 0) {
        file.write(buffer, bytes);
        totalBytes += bytes;
        if (len > 0) {
          len -= bytes;
        }
      }
    }
  }
  
  file.close();
  http.end();
  
  serialPrintf("[Timelapse] Photo saved as %s, %d bytes\n", filename, totalBytes);
  
  timelapsePhotoCount++;
  timelapseLastShotTime = millis();
  
  return true;
}

// 更新timelapse显示界面 (Canvas离屏渲染，防止驱动冲突与消除闪烁)
void updateTimelapseDisplay() {
  static uint32_t lastUpdate = 0;
  if (millis() - lastUpdate < 500) return;
  lastUpdate = millis();

  // 全部在 canvas 离屏画布上绘制，避开与显示驱动底层直接竞争
  canvas.fillScreen(BLACK);
  canvas.setTextColor(TFT_WHITE, TFT_BLACK);
  
  // 显示标题
  canvas.setTextSize(2);
  canvas.setCursor(10, 5);
  canvas.println("Timelapse");
  
  // 显示拍摄数量
  canvas.setTextSize(1);
  canvas.setCursor(10, 35);
  canvas.printf("Photos: %d", timelapsePhotoCount);
  
  // 显示倒计时
  unsigned long timeSinceLastShot = millis() - timelapseLastShotTime;
  long countdown = (long)timelapseInterval - (long)timeSinceLastShot;
  if (countdown < 0) countdown = 0;
  
  canvas.setCursor(10, 55);
  if (timeSinceLastShot >= timelapseInterval) {
    canvas.setTextColor(TFT_YELLOW);
    canvas.println("Status: Capturing...");
  } else {
    canvas.setTextColor(TFT_CYAN);
    canvas.printf("Next Photo: %02ld s", countdown / 1000);
  }
  
  // 右上角信息
  if (isSDInitialized) {
    canvas.setCursor(160, 5);
    canvas.setTextColor(TFT_GREEN);
    canvas.printf("SD:OK");
  }
  
  canvas.setCursor(10, 115);
  canvas.setTextColor(TFT_DARKGREY);
  canvas.println("Hold BtnA to Exit");
  
  // 最终推送到屏幕
  canvas.pushSprite(0, 0);
}





// 启动timelapse模式
void startTimelapseMode() {
  serialPrintf("Starting timelapse mode...\n");
  serialPrintf("isSDInitialized: %d\n", isSDInitialized);
  
  // 创建timelapse目录
  if (!createTimelapseDir()) {
    serialPrintf("Failed to create timelapse directory\n");
    
    // 在屏幕上显示错误提示
    M5Cardputer.Display.clearDisplay();
    M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5Cardputer.Display.setTextSize(2);
    M5Cardputer.Display.setCursor(10, 60);
    M5Cardputer.Display.println("SD Card Error");
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setCursor(10, 90);
    M5Cardputer.Display.println("Please insert SD card");
    M5Cardputer.Display.setCursor(10, 110);
    M5Cardputer.Display.println("Press any key to continue");
    
    // 等待用户按键
    while (true) {
      M5Cardputer.update();
      if (M5Cardputer.Keyboard.isChange()) {
        M5Cardputer.Keyboard.updateKeysState();
        break;
      }
      delay(100);
    }
    
    return;
  }
  
  serialPrintf("Timelapse directory created successfully\n");
  
  // 停止MJPEG流以防止资源冲突
  streamHttp.end();
  streamClient.stop();
  delay(500);
  
  // 设置timelapse分辨率和质量
  serialPrintf("Setting timelapse resolution...\n");
  if (!setCameraResolution(CAMERA_RESOLUTION_TIMELAPSE)) {
    serialPrintf("Failed to set timelapse resolution\n");
    return;
  }
  
  serialPrintf("Setting high quality...\n");
  if (!setCameraQuality(2)) {
    serialPrintf("Failed to set high quality\n");
    return;
  }
  
  // 初始化timelapse状态
  isTimelapseMode = true;
  timelapsePhotoCount = 0;
  timelapseLastShotTime = millis();
  timelapseStartTime = millis();
  lastUserActionTime = millis();
  isScreenOff = false;
  
  serialPrintf("Timelapse state initialized\n");
  serialPrintf("isTimelapseMode: %d\n", isTimelapseMode);
  
  // 清屏
  M5Cardputer.Display.clearDisplay();
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5Cardputer.Display.setTextSize(2);
  M5Cardputer.Display.setCursor(10, 60);
  M5Cardputer.Display.println("Timelapse Mode");
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setCursor(10, 90);
  M5Cardputer.Display.println("Press BtnA to exit");
  delay(2000);
  
  serialPrintf("Timelapse mode started\n");
}

// 停止timelapse模式
void stopTimelapseMode() {
  serialPrintf("Stopping timelapse mode...\n");
  
  isTimelapseMode = false;
  isScreenOff = false;
  
  // 恢复低分辨率和低质量（串流模式）
  serialPrintf("Restoring low resolution...\n");
  setCameraResolution(CAMERA_RESOLUTION_LOW);
  
  serialPrintf("Restoring stream quality...\n");
  setCameraQuality(CAMERA_QUALITY_STREAM);
  
  // 标记需要重启视频流
  appState.isRestartStream = true;
  
  // 清屏
  M5Cardputer.Display.clearDisplay();
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5Cardputer.Display.setTextSize(2);
  M5Cardputer.Display.setCursor(10, 60);
  M5Cardputer.Display.printf("Captured: %d", timelapsePhotoCount);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setCursor(10, 90);
  M5Cardputer.Display.println("Press any key");
  
  delay(2000);
  
  // 清屏以移除统计信息，准备显示视频流
  M5Cardputer.Display.clearDisplay();
  
  // 核心重置：将状态机回滚到接收模式，并清除残留计数
  appState.currentState = STATE_RECEIVING;
  appState.parseState = AppState::P_HTTP_HEADERS;
  appState.networkSize = 0;
  appState.isCaptureReq = false;
  appState.frameReadCount = 0;
  appState.consecutiveErrors = 0;
  
  serialPrintf("Timelapse mode stopped. Total photos: %d\n", timelapsePhotoCount);
}


// 显示状态文件 (重构为格式化显示)
void showStatusFile() {
  if (!isSDInitialized) return;
  
  File file = SD.open("/camera_status.json");
  if (!file) {
    displayLine("No status file found");
    delay(1000);
    return;
  }
  
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  // 强制去抖：等待按键完全释放 (Edge Trigger)
  delay(100);
  while (M5Cardputer.Keyboard.isPressed()) {
    M5Cardputer.update();
    delay(10);
  }

  M5Cardputer.Display.clearDisplay();

  M5Cardputer.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
  M5Cardputer.Display.setTextSize(2);
  M5Cardputer.Display.setCursor(10, 5);
  M5Cardputer.Display.println("Camera Status");
  
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  
  if (error) {
    M5Cardputer.Display.setCursor(10, 40);
    M5Cardputer.Display.println("JSON Parse Error");
  } else {
    int y = 35;
    auto printParam = [&](const char* label, const char* key) {
      M5Cardputer.Display.setCursor(10, y);
      M5Cardputer.Display.printf("%-12s: %s", label, doc[key].as<String>().c_str());
      y += 15;
    };
    
    printParam("Resolution", "framesize");
    printParam("Quality", "quality");
    printParam("Brightness", "brightness");
    printParam("Contrast", "contrast");
    printParam("Saturation", "saturation");
    printParam("SpecialEff", "special_effect");
    printParam("AWB", "awb");
    printParam("AEC", "aec");
  }
  
  M5Cardputer.Display.setCursor(10, 115);
  M5Cardputer.Display.setTextColor(TFT_DARKGREY);
  M5Cardputer.Display.println("Press ANY Key to Return");
  
  // 等待用户退出 (边沿触发)
  while (true) {
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      break;
    }
    delay(50);
  }
  
  // 退出前再次等待释放，防止主循环接管长按
  while (M5Cardputer.Keyboard.isPressed()) {
    M5Cardputer.update();
    delay(10);
  }
}


// 显示帮助菜单
void showHelp() {
  M5Cardputer.Display.clearDisplay();
  M5Cardputer.Display.setTextColor(TFT_GREEN, TFT_BLACK);
  M5Cardputer.Display.setTextSize(2);
  M5Cardputer.Display.setCursor(10, 5);
  M5Cardputer.Display.println("Controls Help");
  
  // 强制去抖：等待按键完全释放 (Edge Trigger)
  delay(100);
  while (M5Cardputer.Keyboard.isPressed()) {
    M5Cardputer.update();
    delay(10);
  }

  M5Cardputer.Display.setTextSize(1);


  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  
  const char* helpText[] = {
    "BtnA: Capture Photo / Exit Timelapse",
    "T   : Enter Timelapse Mode",
    "R   : Restart Device",
    "ESC : View Camera Status",
    "H   : Show this help",
    ";/. : Brightness +/-",
    "//, : Contrast +/-",
    "[/] : Saturation +/-",
    "_/= : Sharpness +/-",
    "0-6 : Special Effects"
  };
  
  for (int i = 0; i < 10; i++) {
    M5Cardputer.Display.setCursor(10, 35 + i * 10);
    M5Cardputer.Display.println(helpText[i]);
  }
  
  M5Cardputer.Display.setCursor(10, 115);
  M5Cardputer.Display.setTextColor(TFT_DARKGREY);
  M5Cardputer.Display.println("Press ANY Key to Return");
  
  while (true) {
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      break;
    }
    delay(50);
  }

  // 退出前再次等待释放
  while (M5Cardputer.Keyboard.isPressed()) {
    M5Cardputer.update();
    delay(10);
  }
}



// 初始化硬件
void initHardware() {
  M5Cardputer.begin();
  Serial.begin(115200);
  
  // 初始化LCD显示
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.fillScreen(BLACK);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(WHITE);
  
  displayLine("Initializing SD card...", true); // 清屏并重新开始打印
  Serial.println("Initializing SD card...");
  
  // M5Cardputer SD卡引脚配置
  const int SD_CS = 12;
  const int SD_SCK = 40;
  const int SD_MISO = 39;
  const int SD_MOSI = 14;
  
  // 使用自定义SPI引脚初始化SD卡
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  
  if (!SD.begin(SD_CS)) {

    displayLine("SD card init failed!");
    Serial.println("SD card initialization failed!");
    isSDInitialized = false;
  } else {
    isSDInitialized = true;
    displayLine("SD card initialized!");
    Serial.println("SD card initialized successfully!");
  }
}

// 初始化WiFi
bool initWiFi() {
  displayLine("Connecting WiFi...");
  WiFi.begin("UnitCamS3-WiFi", "");
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    delay(500);
    Serial.print(".");
    M5Cardputer.Display.print(".");
    retry++;
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    displayLine("");
    displayLine("WiFi connect failed!");
    return false;
  }
  displayLine("WiFi connected!");
  displayLine("IP: " + WiFi.localIP().toString());
  
  if (!setCameraResolution(CAMERA_RESOLUTION_LOW)) return false;
  if (!setCameraQuality(CAMERA_QUALITY_STREAM)) return false;
  getCameraConfig();
  loadCameraStatus();
  return true;
}

// 处理子字节流的 MJPEG 解析
void handlePureMjpegByte(uint8_t c) {
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

        // 强力对齐：只识别 EOI (FF D9)
        if (appState.networkSize >= 2 && 
            appState.networkBuffer[appState.networkSize-2] == 0xFF && 
            appState.networkBuffer[appState.networkSize-1] == 0xD9) {
          
          if (appState.networkSize >= 2048) {
            appState.currentState = STATE_DISPLAYING;
          } else {
            appState.networkSize = 0; // 丢弃过小的噪音帧
          }
          appState.parseState = AppState::P_BOUNDARY;
        } else if (appState.frameReadCount >= 15000) {
          Serial.print("B"); // 帧大小溢出/同步错误熔断
          appState.networkSize = 0;
          appState.parseState = AppState::P_BOUNDARY;
        }
      }
      break;
  }
}

void processMjpegStream(WiFiClient& client, bool forceReset = false) {
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
    Serial.println("MJPEG Parser Force Reset");
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
      case CS_SIZE:
        if (c == '\n') {
          if (sizeBuf.length() > 0) {
            chunkSize = strtol(sizeBuf.c_str(), NULL, 16);
            if (chunkSize == 0) { client.stop(); return; }
            cState = CS_DATA;
          }
          sizeBuf = "";
        } else if (isxdigit(c)) sizeBuf += (char)c;
        break;

      case CS_DATA:
        handlePureMjpegByte(c);
        if (chunkSize > 0) chunkSize--;
        if (chunkSize == 0) cState = CS_TRAILER;
        if (appState.currentState == STATE_DISPLAYING) return;
        break;

      case CS_TRAILER:
        if (c == '\n') cState = CS_SIZE;
        break;
    }
  }
}




// 主循环
void loop() {
  M5Cardputer.update();
  
  // Timelapse模式处理
  if (isTimelapseMode) {
    // 检测任意按键（键盘和BtnA）
    bool anyKeyPressed = M5Cardputer.Keyboard.isChange() || M5Cardputer.BtnA.wasPressed();
    
    if (anyKeyPressed) {
      M5Cardputer.Keyboard.updateKeysState();
      
      // 如果屏幕熄灭，先点亮屏幕
      if (isScreenOff) {
        isScreenOff = false;
        M5Cardputer.Display.wakeup();
        lastUserActionTime = millis();
        updateTimelapseDisplay();
      } else {
        // 更新最后操作时间
        lastUserActionTime = millis();
        
        // 处理BtnA退出timelapse模式（只在屏幕点亮状态下）
        if (M5Cardputer.BtnA.wasPressed()) {
          stopTimelapseMode();
          return;
        }
      }
    }
    
    // 检查是否需要息屏（1分钟无操作）
    if (!isScreenOff && millis() - lastUserActionTime >= screenOffTimeout) {
      isScreenOff = true;
      M5Cardputer.Display.sleep();
    }
    
    // 更新timelapse显示界面（只在屏幕点亮时）
    if (!isScreenOff) {
      updateTimelapseDisplay();
    }
    
    // 检查是否需要拍摄照片（5秒间隔）
    unsigned long timeSinceLastShot = millis() - timelapseLastShotTime;
    if (timeSinceLastShot >= timelapseInterval) {
      serialPrintf("[Timelapse] Time since last shot: %lu ms, triggering capture\n", timeSinceLastShot);
      captureTimelapsePhoto();
    }
    
    delay(100);
    return;
  }
  
  // 处理用户按键
  if (M5Cardputer.Keyboard.isChange()) {
    M5Cardputer.Keyboard.updateKeysState();
    serialPrintf("Keyboard state changed\n");
    
    // 处理重启键（只在按键变化时触发一次）
    if (M5Cardputer.Keyboard.isKeyPressed('r')) {
      // logLine("User requested device restart");
      M5Cardputer.Display.fillScreen(BLACK);
      M5Cardputer.Display.setCursor(10, 30);
      M5Cardputer.Display.setTextSize(2);
      M5Cardputer.Display.println("Restarting device...");
      delay(1000);
      ESP.restart();
    }
    
    // 处理t键启动timelapse模式
    if (M5Cardputer.Keyboard.isKeyPressed('t')) {
      serialPrintf("t key pressed, starting timelapse mode...\n");
      startTimelapseMode();
      return;
    }
    
    // 处理数字键0-6，设置相机特效（只在按键变化时触发一次）
    for (int i = 0; i <= 6; i++) {
      char key = '0' + i;
      if (M5Cardputer.Keyboard.isKeyPressed(key)) {
        setCameraSpecialEffect(i);
        break;
      }
    }
    
    // 处理显示状态信息 (ESC)
    if (M5Cardputer.Keyboard.isKeyPressed('`')) { // '`' is ESC
      showStatusFile();
    }
    
    // 处理显示说明 (h键)
    if (M5Cardputer.Keyboard.isKeyPressed('h')) {
      showHelp();
    }
  }

  
  // 处理参数调节按键（持续检测，带防抖动）
  unsigned long currentTime = millis();
  if (currentTime - lastKeyPressTime >= keyDebounceDelay) {
    bool keyPressed = false;
    
    // 处理亮度调节（; 上键增加，. 下键减少）
    if (M5Cardputer.Keyboard.isKeyPressed(';')) {
      if (currentBrightness < 2) {
        currentBrightness++;
        setCameraParameter("brightness", currentBrightness);
        keyPressed = true;
      }
    } else if (M5Cardputer.Keyboard.isKeyPressed('.')) {
      if (currentBrightness > -2) {
        currentBrightness--;
        setCameraParameter("brightness", currentBrightness);
        keyPressed = true;
      }
    }
    
    // 处理对比度调节（, 左键减少，/ 右键增加）
    if (!keyPressed && M5Cardputer.Keyboard.isKeyPressed(',')) {
      if (currentContrast > -2) {
        currentContrast--;
        setCameraParameter("contrast", currentContrast);
        keyPressed = true;
      }
    } else if (!keyPressed && M5Cardputer.Keyboard.isKeyPressed('/')) {
      if (currentContrast < 2) {
        currentContrast++;
        setCameraParameter("contrast", currentContrast);
        keyPressed = true;
      }
    }
    
    // 处理饱和度调节（[ 左中括号减少，] 右中括号增加）
    if (!keyPressed && M5Cardputer.Keyboard.isKeyPressed('[')) {
      if (currentSaturation > -2) {
        currentSaturation--;
        setCameraParameter("saturation", currentSaturation);
        keyPressed = true;
      }
    } else if (!keyPressed && M5Cardputer.Keyboard.isKeyPressed(']')) {
      if (currentSaturation < 2) {
        currentSaturation++;
        setCameraParameter("saturation", currentSaturation);
        keyPressed = true;
      }
    }
    
    // 处理锐度调节（_ 下划线减少，= 等号增加）
    if (!keyPressed && M5Cardputer.Keyboard.isKeyPressed('_')) {
      if (currentSharpness > -2) {
        currentSharpness--;
        setCameraParameter("sharpness", currentSharpness);
        keyPressed = true;
      }
    } else if (!keyPressed && M5Cardputer.Keyboard.isKeyPressed('=')) {
      if (currentSharpness < 2) {
        currentSharpness++;
        setCameraParameter("sharpness", currentSharpness);
        keyPressed = true;
      }
    }
    
    // 如果有按键被按下，更新最后按键时间
    if (keyPressed) {
      lastKeyPressTime = currentTime;
    }
  }
  
  // 处理BtnA按下（拍照）
  if (M5Cardputer.BtnA.wasPressed()) {
    appState.isCaptureReq = true;
  }
  
  // 处理拍摄请求
  if (appState.isCaptureReq) {
    appState.isCaptureReq = false;
    // logLine("Processing capture request...");
    if (captureSnapshot()) {
      // logLine("Capture successful");
      
      // 保存照片到SD卡
      if (isSDInitialized) {
        // logLine("Saving photo to SD card...");
        
        // 获取当前缓冲区的数据 (直接使用 networkBuffer)
        if (appState.networkSize > 0) {
          // 创建带时间戳的文件名
          time_t now = time(nullptr);
          struct tm *timeinfo = localtime(&now);
          char filename[40];
          sprintf(filename, "/images/IMG_%04d%02d%02d_%02d%02d%02d.jpg", 
                  timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday,
                  timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
          
          // 打开文件进行写入
          File file = SD.open(filename, FILE_WRITE);
          if (!file) {
            // logLine("Failed to open file");
          } else {
            // 写入JPEG数据
            size_t bytesWritten = file.write(appState.networkBuffer, appState.networkSize);
            if (bytesWritten != appState.networkSize) {
              // logLine("Failed to write to file");
            } else {
              // 成功保存照片，显示 Overlay 提示
              snprintf(appState.overlayMsg, sizeof(appState.overlayMsg), "Saved: %s", filename + 8); // 跳过 "/images/"
              appState.overlayTimestamp = millis();
              Serial.printf("Photo saved: %s\n", filename);
            }
            file.close();
          }
        }
      } else {
        snprintf(appState.overlayMsg, sizeof(appState.overlayMsg), "No SD Card!");
        appState.overlayTimestamp = millis();
      }

    } else {
      // logLine("Capture failed");
    }
  }
  
  // 检查WiFi连接状态
  if (WiFi.status() == WL_CONNECTED) {
    if (!streamClient.connected()) {
      if (appState.isRestartStream) {
        appState.isRestartStream = false;
        
        // 清除图像尺寸缓存（因为流重启了）
        appState.sizeCached = false;
        appState.cachedImgWidth = 0;
        appState.cachedImgHeight = 0;
        appState.boundaryFound = false;
        appState.boundary = "";
        
        // 重置缓冲区
        appState.networkSize = 0;
        appState.currentState = STATE_RECEIVING;
        
        streamClient.stop();
        streamHttp.end();
        
        // 等待相机完成分辨率切换
        delay(500);
        
        // 使用原始WiFiClient连接MJPEG流
        Serial.println("Connecting to MJPEG stream...");
        if (streamClient.connect("192.168.4.1", 80)) {
          // 强制重置解析器内部静态状态机
          processMjpegStream(streamClient, true);
          
          // 发送HTTP模拟浏览器请求 (Ref: har-requests.txt)




          String request = "GET /api/v1/stream HTTP/1.1\r\n";
          request += "Host: 192.168.4.1\r\n";
          request += "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/143.0.0.0 Safari/537.36\r\n";
          request += "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7\r\n";
          request += "Referer: http://192.168.4.1/\r\n";
          request += "Accept-Language: zh-CN,zh;q=0.9,en;q=0.8\r\n";
          request += "Connection: keep-alive\r\n";
          request += "\r\n";

          streamClient.print(request);
          Serial.println("MJPEG stream connected successfully. Syncing handshake...");
          
          // 强对齐同步：等待 HTTP 响应头结束 (\r\n\r\n)
          unsigned long syncStart = millis();
          String syncBuf = "";
          bool syncOk = false;
          while (millis() - syncStart < 3000) {
            if (streamClient.available()) {
              char sc = streamClient.read();
              syncBuf += sc;
              // 寻找 HTTP 报文体起始标志
              if (syncBuf.endsWith("\r\n\r\n") || syncBuf.endsWith("\n\n")) {
                syncOk = true;
                break;
              }
              if (syncBuf.length() > 200) syncBuf = syncBuf.substring(150); 
            }
          }
          
          if (syncOk) {
            Serial.println("MJPEG Protocol Body Sync OK.");
          } else {
            Serial.println("MJPEG Protocol Sync Timeout. Proceeding anyway...");
          }

          
          streamClient.setNoDelay(true);
          streamClient.setTimeout(5000);

        } else {
          Serial.println("Failed to connect to MJPEG stream");
          delay(2000);
          return;
        }
      }
    } else {
      // 处理流数据
      processMjpegStream(streamClient);
    }
  } else {
    // WiFi未连接，停止当前连接
    if (streamClient.connected()) {
      streamClient.stop();
      streamHttp.end();
    }
    // 每5秒检查一次WiFi状态
    static unsigned long lastWifiCheck = 0;
    if (millis() - lastWifiCheck > 5000) {
      lastWifiCheck = millis();
      // logLine("WiFi disconnected, waiting for network recovery...");
    }
  }
  
  if (appState.currentState == STATE_DISPLAYING && (millis() - lastDisplayTime >= minFrameInterval)) {
    // 更新最后显示时间
    lastDisplayTime = millis();
    
    // 直接使用 appState.networkBuffer 进行绘图 (省下一份 64KB displayBuffer 拷贝耗时和空间)
    // 解析JPEG尺寸，计算居中裁切坐标
    int jpegW = 0, jpegH = 0;
    int drawX = 0, drawY = 0;
    int dispW = M5Cardputer.Display.width();
    int dispH = M5Cardputer.Display.height();
    
    if (parseJpegSize(appState.networkBuffer, appState.networkSize, jpegW, jpegH) && jpegW > 0 && jpegH > 0) {
      drawX = (dispW - jpegW) / 2;
      drawY = (dispH - jpegH) / 2;
    }
    
    // 渲染 JPEG
    bool drawSuccess = canvas.drawJpg(appState.networkBuffer, appState.networkSize, drawX, drawY);
    
    // 合成 Overlay
    {
      static uint32_t cachedUsedKB = 0;
      static uint32_t cachedTotalKB = 0;
      if (millis() - lastOverlayUpdate >= overlayInterval) {
        lastOverlayUpdate = millis();
        cachedUsedKB  = (ESP.getHeapSize() - ESP.getFreeHeap()) / 1024;
        cachedTotalKB = ESP.getHeapSize() / 1024;
      }
      char overlayBuf[32];
      snprintf(overlayBuf, sizeof(overlayBuf), "FPS:%.1f %u/%uKB",
               currentFps, cachedUsedKB, cachedTotalKB);
      canvas.setTextColor(TFT_WHITE);
      canvas.setTextSize(1);
      canvas.setCursor(115, 2);
      canvas.print(overlayBuf);
    }
    
    // 渲染拍照后的浮动提示 (Overlay)
    if (appState.overlayTimestamp > 0) {
      if (millis() - appState.overlayTimestamp < 2000) {
        // 在底部渲染半透明背板感或简单的对比色文字
        canvas.setTextColor(TFT_YELLOW);
        canvas.setTextSize(1);
        int msgWidth = canvas.textWidth(appState.overlayMsg);
        canvas.setCursor((240 - msgWidth) / 2, 120);
        canvas.print(appState.overlayMsg);
      } else {
        appState.overlayTimestamp = 0; // 超时重置
      }
    }
    
    // 推送到屏幕

    if (drawSuccess) {
      canvas.pushSprite(0, 0);
      fpsFrameCount++;
      appState.consecutiveErrors = 0; // 重置错误计数
      appState.lastValidSize = (int)appState.networkSize; // 记录上一帧有效载荷大小
    } else {

      appState.consecutiveErrors++;
      uint8_t tail1 = (appState.networkSize >= 2) ? appState.networkBuffer[appState.networkSize-2] : 0;
      uint8_t tail2 = (appState.networkSize >= 1) ? appState.networkBuffer[appState.networkSize-1] : 0;
      Serial.printf("E(S:%d,H:%02x%02x,T:%02x%02x)", appState.networkSize, appState.networkBuffer[0], appState.networkBuffer[1], tail1, tail2);
      
      // 自愈逻辑：如果连续 15 帧渲染失败，说明流失步严重，强制重连
      if (appState.consecutiveErrors >= 15) {
        Serial.println("\n[Auto-Healing] Too many errors, restarting stream...");
        appState.isRestartStream = true;
        appState.consecutiveErrors = 0;
      }
    }




    // 强制同步等待
    M5Cardputer.Display.flush();
    delay(1);

    
    // 显示彻底完成，清空网络缓冲的大小记录，并恢复接收状态，允许下一帧的读取
    appState.networkSize = 0;
    appState.currentState = STATE_RECEIVING;

  }
  
  // 计算帧率（每1秒输出一次串口诊断信息）
  if (millis() - fpsLastTime >= 1000) {
    currentFps = (float)fpsFrameCount / ((millis() - fpsLastTime) / 1000.0);
    fpsFrameCount = 0;
    fpsLastTime = millis();
    Serial.printf("FPS: %.1f  FREE: %uKB  Size: %d B\n",
                  currentFps,
                  ESP.getFreeHeap() / 1024,
                  appState.lastValidSize);


  }


  //delay(10);
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n\n--- [BOOT UP] M5Cardputer Camera Interface Starting... ---");
  
  // 初始化应用状态
  initAppState();

  
  // 初始化硬件和基础显示（会清屏并从头设置displayLine位置）
  initHardware();
  
  // 初始化离屏画布 (240x135)
  canvas.createSprite(M5Cardputer.Display.width(), M5Cardputer.Display.height());
  canvas.setTextScroll(true); // 允许画布文字滚动

  
  // 显示应用标题
  displayLine("Camera App Started");
  
  if (!initWiFi()) {
    displayLine("WiFi connect failed");
    displayLine("please check network status");
    displayLine("press R to restart");
  } else {
    // 显示初始化完成信息
    displayLine("Ready!");
    
    // 清屏，立即准备显示流画面
    M5Cardputer.Display.fillScreen(BLACK);
    
    // 启动MJPEG流
    appState.isRestartStream = true;
  }
}
