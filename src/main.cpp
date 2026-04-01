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
#define CAMERA_QUALITY_TIMELAPSE 2  // timelapse拍摄画质（与串流保持一致）

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

  // Chunked 解码器状态
  enum ChunkState { CS_SIZE, CS_DATA, CS_TRAILER };
  ChunkState cState;
  uint32_t chunkSize;
  String sizeBuf;
  unsigned long lastAct;

  // 读取缓存 (1024 字节)
  uint8_t readCache[1024];
  int cachePos;
  int cacheLen;
  
  // 网络状态
  bool boundaryFound;
  String boundary;
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

  // 初始化 Chunked 解析器状态
  appState.cState = AppState::CS_SIZE;
  appState.chunkSize = 0;
  appState.sizeBuf = "";
  appState.lastAct = 0;

  // 初始化缓存状态
  appState.cachePos = 0;
  appState.cacheLen = 0;
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
  M5Cardputer.Display.setCursor(10, 100);
  M5Cardputer.Display.printf("Setting camera resolution to %d...\n", resolution);
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
  // logLine("Camera resolution set successfully");
  M5Cardputer.Display.println("Camera resolution set!");
  
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
  
  // 获取最新图像数据
  HTTPClient http;
  
  // 第一次请求：触发拍摄（忽略返回的旧图像数据）
  const char* captureUrl = "http://192.168.4.1/api/v1/capture";
  http.begin(captureUrl);
  http.addHeader("User-Agent", "M5Cardputer");
  http.setTimeout(15000);
  
  serialPrintf("[Timelapse] First request (trigger): GET %s\n", captureUrl);
  int code = http.GET();
  
  // 关闭第一次请求
  http.end();
  
  // 等待相机处理新图像
  delay(500);
  
  // 第二次请求：获取新的图像数据
  http.begin(captureUrl);
  http.addHeader("User-Agent", "M5Cardputer");
  http.setTimeout(15000);
  
  serialPrintf("[Timelapse] Second request (fetch): GET %s\n", captureUrl);
  code = http.GET();
  
  if (code != 200) {
    serialPrintf("[Timelapse] HTTP %d\n", code);
    http.end();
    // 即使拍摄失败，也要重置倒计时，避免卡在0秒
    timelapseLastShotTime = millis();
    return false;
  }
  
  String ct = http.header("Content-Type");
  serialPrintf("[Timelapse] CT: %s\n", ct.c_str());
  
  // 验证内容类型是否为JPEG，但允许空内容类型（相机API可能不设置它）
  if (!ct.isEmpty() && !ct.startsWith("image/jpeg")) {
    serialPrintf("[Timelapse] Unexpected content-type: %s\n", ct.c_str());
    http.end();
    timelapseLastShotTime = millis();
    return false;
  }
  
  // 读取JPEG数据
  WiFiClient* s = http.getStreamPtr();
  s->setNoDelay(true);
  s->setTimeout(10000);
  int len = http.getSize();
  
  serialPrintf("[Timelapse] Content length: %d\n", len);
  
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
  
  // 创建带序号的文件名
  char filename[40];
  sprintf(filename, "%s/IMG_%04d.jpg", currentTimelapseDir.c_str(), maxPhotoNum + 1);
  
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

// 更新timelapse显示界面
void updateTimelapseDisplay() {
  // 清除屏幕
  M5Cardputer.Display.fillScreen(BLACK);
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  
  // 显示标题
  M5Cardputer.Display.setTextSize(2);
  M5Cardputer.Display.setCursor(10, 5);
  M5Cardputer.Display.println("Timelapse Mode");
  M5Cardputer.Display.setTextSize(1);
  
  // 显示拍摄数量
  M5Cardputer.Display.setCursor(10, 30);
  M5Cardputer.Display.printf("Photos: %d", timelapsePhotoCount);
  
  // 显示倒计时
  unsigned long timeSinceLastShot = millis() - timelapseLastShotTime;
  unsigned long countdown = timelapseInterval - timeSinceLastShot;
  if (countdown < 0) countdown = 0;
  
  M5Cardputer.Display.setCursor(5, 20);
  if (timeSinceLastShot >= timelapseInterval) {
    M5Cardputer.Display.fillRect(5, 20, 100, 10, TFT_BLACK);
    M5Cardputer.Display.printf("Capturing");
  } else {
    M5Cardputer.Display.fillRect(5, 20, 100, 10, TFT_BLACK);
    M5Cardputer.Display.printf("Next: %ds", countdown / 1000);
  }
  
  // 右上角：存储卡剩余容量和电量百分比
  uint64_t freeSpace = SD.totalBytes() - SD.usedBytes();
  float freeSpaceMB = freeSpace / (1024.0f * 1024.0f);
  int battery = M5Cardputer.Power.getBatteryLevel();
  
  String spaceStr = String(freeSpaceMB, 1) + "MB";
  int spaceStrWidth = M5Cardputer.Display.textWidth(spaceStr);
  M5Cardputer.Display.setCursor(320 - spaceStrWidth - 5, 5);
  M5Cardputer.Display.printf("%s", spaceStr.c_str());
  
  String batteryStr = String(battery) + "%";
  int batteryStrWidth = M5Cardputer.Display.textWidth(batteryStr);
  M5Cardputer.Display.setCursor(320 - batteryStrWidth - 5, 20);
  M5Cardputer.Display.printf("%s", batteryStr.c_str());
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
  
  serialPrintf("Timelapse mode stopped. Total photos: %d\n", timelapsePhotoCount);
}

// 显示状态文件
void showStatusFile() {
  if (!isSDInitialized) return;
  
  File file = SD.open("/camera_status.json");
  if (!file) return;
  
  M5Cardputer.Display.clearDisplay();
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5Cardputer.Display.setTextSize(1);
  
  int lineCount = 0;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    M5Cardputer.Display.setCursor(10, 10 + lineCount * 12);
    M5Cardputer.Display.println(line);
    lineCount++;
    if (lineCount > 15) break;
  }
  
  file.close();
  
  // 显示滚动提示
  M5Cardputer.Display.setCursor(10, 220);
  M5Cardputer.Display.println("Press ESC to exit");
  
  // 等待用户退出
  bool isShowingStatus = true;
  int statusScrollOffset = 0;
  while (isShowingStatus) {
    M5Cardputer.update();
    
    if (M5Cardputer.Keyboard.isChange()) {
      M5Cardputer.Keyboard.updateKeysState();
      
      // 按`键退出
      if (M5Cardputer.Keyboard.isKeyPressed('`')) {
        isShowingStatus = false;
      }
      
      // 上键滚动
      if (M5Cardputer.Keyboard.isKeyPressed(';')) {
        if (statusScrollOffset > 0) {
          statusScrollOffset--;
        }
      }
      
      // 下键滚动
      if (M5Cardputer.Keyboard.isKeyPressed('.')) {
        if (statusScrollOffset < lineCount - 15) {
          statusScrollOffset++;
        }
      }
    }
  }
  
  // 退出后清空屏幕并重启视频流
  M5Cardputer.Display.fillScreen(BLACK);
  
  // 标记需要重启视频流
  appState.isRestartStream = true;
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
          appState.parseState = AppState::P_JPEG_DATA; appState.networkSize = 0; appState.frameReadCount = 0;
        } else if (header.startsWith("content-length:")) {
          String val = header.substring(15);
          val.trim();
          appState.expectedCL = val.toInt();
        }
        appState.lineBuffer = "";
      } else if (c != '\r') appState.lineBuffer += (char)c;
      break;


    case AppState::P_JPEG_DATA:
      // 首先寻找 SOI (FF D8)
      if (appState.networkSize == 0) {
        if (c == 0xFF) {
          appState.networkBuffer[appState.networkSize++] = c;
        }
        // 如果不是 FF，则丢弃该字节，直到找到 JPEG 起始
      } else if (appState.networkSize == 1) {
        if (c == 0xD8) {
          appState.networkBuffer[appState.networkSize++] = c;
        } else if (c == 0xFF) {
          // 连续的 FF，维持 networkSize = 1 寻找 D8
        } else {
          // 错误的起始，重置
          appState.networkSize = 0;
        }
      } else {
        // 已有 SOI，开始填充直到 EOI 或 长度到达
        if (appState.networkSize < GLOBAL_MAX_JPEG_SIZE) {
          appState.networkBuffer[appState.networkSize++] = c;
        }
        appState.frameReadCount++;
        
        bool isFinished = false;
        // 1. 扫描 EOI (FF D9) - 这是最可靠的帧尾标志
        if (appState.networkSize >= 2 && 
            appState.networkBuffer[appState.networkSize-2] == 0xFF && 
            appState.networkBuffer[appState.networkSize-1] == 0xD9) {
          isFinished = true;
        }
        // 2. 备选：如果根据长度计数已到达且已经有基本大小
        else if (appState.expectedCL > 0 && appState.frameReadCount >= appState.expectedCL) {
          isFinished = true;
        }

        if (isFinished) {
          // 最终校验：必须以 FF D8 开头且长度足够
          if (appState.networkSize >= 1024 && appState.networkBuffer[0] == 0xFF && appState.networkBuffer[1] == 0xD8) {
            appState.currentState = STATE_DISPLAYING;
          } else {
            // 虽然判为结束但内容显然不完整
            if (appState.networkSize > 2) Serial.print("?");
            appState.networkSize = 0;
          }
          appState.parseState = AppState::P_BOUNDARY;
        } else if (appState.frameReadCount >= GLOBAL_MAX_JPEG_SIZE) {
          Serial.print("O"); // Overflow
          appState.networkSize = 0;
          appState.parseState = AppState::P_BOUNDARY;
        }
      }
      break;
  }
}

void processMjpegStream(WiFiClient& client) {
  if (appState.currentState == STATE_DISPLAYING) return;
  
  // 积压防御
  if (client.available() > 20480) {
    while (client.available() > 4096) client.read();
    appState.cachePos = 0; appState.cacheLen = 0; // 清除缓存
    appState.cState = AppState::CS_SIZE;
    appState.sizeBuf = "";
    appState.parseState = AppState::P_BOUNDARY;
    return;
  }

  // 循环尝试从网络或缓存读取数据
  while (true) {
    // 1. 如果缓存空了，尝试从网络补充
    if (appState.cachePos >= appState.cacheLen) {
      if (client.available() == 0) break; // 网络无更多数据，退回主循环
      appState.cachePos = 0;
      appState.cacheLen = client.read(appState.readCache, sizeof(appState.readCache));
      if (appState.cacheLen <= 0) break;
    }

    // 2. 取出一个字节并应用状态机
    uint8_t c = appState.readCache[appState.cachePos++];
    appState.lastAct = millis();

    switch (appState.cState) {
      case AppState::CS_SIZE:
        if (c == '\n') {
          if (appState.sizeBuf.length() > 0) {
            appState.chunkSize = strtol(appState.sizeBuf.c_str(), NULL, 16);
            if (appState.chunkSize == 0) { client.stop(); return; }
            appState.cState = AppState::CS_DATA;
          }
          appState.sizeBuf = "";
        } else if (isxdigit(c)) appState.sizeBuf += (char)c;
        break;

      case AppState::CS_DATA:
        handlePureMjpegByte(c);
        if (appState.chunkSize > 0) appState.chunkSize--;
        if (appState.chunkSize == 0) appState.cState = AppState::CS_TRAILER;
        
        // 如果 handlePureMjpegByte 完成了一帧，这里必须立即退出以显示
        if (appState.currentState == STATE_DISPLAYING) return;
        break;

      case AppState::CS_TRAILER:
        if (c == '\n') appState.cState = AppState::CS_SIZE;
        break;
    }
  }

  if (appState.lastAct > 0 && millis() - appState.lastAct > 3000) {
    client.stop(); appState.lastAct = millis();
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
    
    // 处理显示状态信息（只在按键变化时触发一次）
    if (M5Cardputer.Keyboard.isKeyPressed('`')) {
      showStatusFile();
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

              // logLine(String("Photo saved successfully: ") + filename);
              M5Cardputer.Display.setCursor(10, 10);
              M5Cardputer.Display.println(String("Photo saved: ") + filename);
            }
            file.close();
          }
        }
      } else {
        // logLine("SD card not initialized, cannot save photo");
        M5Cardputer.Display.setCursor(10, 10);
        M5Cardputer.Display.println("SD card not initialized");
      }
    } else {
      // logLine("Capture failed");
    }
  }
  
  // 检查WiFi连接状态
  if (WiFi.status() == WL_CONNECTED) {
    if (!streamClient.connected()) {
        if (appState.isRestartStream) {
          initAppState();
          
          streamClient.stop();
          streamHttp.end();

        
        // 等待相机完成分辨率切换
        delay(500);
        
        // 使用原始WiFiClient连接MJPEG流
        Serial.println("Connecting to MJPEG stream...");
        if (streamClient.connect("192.168.4.1", 80)) {
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
          streamClient.setNoDelay(true);
          streamClient.setTimeout(5000);
          Serial.println("MJPEG stream connected successfully");
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
    
    // 推送到屏幕
    if (drawSuccess) {
      canvas.pushSprite(0, 0);
      fpsFrameCount++;
    } else {
      Serial.print("E");
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
    Serial.printf("FPS: %.1f  FREE: %uKB  Size: %u B\n",
                  currentFps,
                  ESP.getFreeHeap() / 1024,
                  (unsigned)appState.networkSize);

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
    displayLine("Initialization completed!");
    
    // 短暂显示提示信息
    delay(1000);
    
    // 清屏，准备显示流画面
    M5Cardputer.Display.fillScreen(BLACK);
    
    // 显示操作提示
    M5Cardputer.Display.setCursor(10, 5);
    M5Cardputer.Display.setTextColor(WHITE);
    M5Cardputer.Display.println("Press BtnA to capture");
    
    // 启动MJPEG流
    appState.isRestartStream = true;
  }
}