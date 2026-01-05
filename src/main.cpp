#include <M5Cardputer.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <HTTPUpdate.h>
#include <SPI.h>
#include <SD.h>
#include <cstring>
#include <time.h>

// 全局配置
#define GLOBAL_MAX_JPEG_SIZE 70 * 1024 // 75KB最大JPEG尺寸，进一步减小以节省内存

// 相机分辨率常量
#define CAMERA_RESOLUTION_HIGH 13     // 高分辨率 (1280*720)，用于拍摄照片
#define CAMERA_RESOLUTION_LOW 6       // 低分辨率(320*240)，用于实时预览

// 日志相关定义
const char* LOG_HDR_KEYS[] = {"Server", "Content-Type", "Content-Length", "Cache-Control", "Connection"};
constexpr size_t LOG_HDR_KEYS_COUNT = sizeof(LOG_HDR_KEYS) / sizeof(LOG_HDR_KEYS[0]);

// 应用状态
typedef struct {
  bool isCaptureReq;        // 拍摄请求标志
  bool isRestartStream;     // 重启流请求标志
  bool jpegReady;           // JPEG数据就绪标志
  
  // 使用静态数组代替vector作为JPEG数据缓冲区
  uint8_t jpegData[GLOBAL_MAX_JPEG_SIZE];
  size_t jpegDataSize;
} AppState;

AppState appState = {
  false,                   // isCaptureReq
  false,                   // isRestartStream
  false,                   // jpegReady
  {0},                     // jpegData
  0                        // jpegDataSize
};

// SD卡状态全局变量
bool isSDInitialized = false;

// 全局MJPEG流变量
WiFiClient streamClient;
HTTPClient streamHttp;

// 日志函数
void logLine(const String& line) {
  Serial.println(line);
  // 可在此处添加LCD显示逻辑
}

// 记录HTTP请求头
void logHttpRequestHeaders(const String& prefix, const String& url, const std::vector<std::pair<String, String>>& headers) {
  String logStr = String("[") + prefix + "] Request: " + url;
  logLine(logStr);
  for (const auto& header : headers) {
    logStr = String("[") + prefix + "] Header: " + header.first + ": " + header.second;
    logLine(logStr);
  }
}

// 记录HTTP响应头
void logHttpResponseHeaders(const String& prefix, int code, HTTPClient& http) {
  String logStr = String("[") + prefix + "] Response: HTTP " + code;
  logLine(logStr);
  for (const char* key : LOG_HDR_KEYS) {
    String value = http.header(key);
    if (value.length() > 0) {
      logStr = String("[") + prefix + "] Header: " + key + ": " + value;
      logLine(logStr);
    }
  }
}

// 记录原始串口数据
void serialPrintf(const char* format, ...) {
  char buffer[256];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  Serial.print(buffer);
}

// 从数据中解析JPEG尺寸
bool parseJpegSize(const uint8_t* data, size_t size, int& width, int& height) {
  if (size < 2) {
    return false;
  }

  // 查找SOF标记 (SOF0或SOF2)
  for (size_t i = 0; i < size - 1; ++i) {
    if (data[i] == 0xFF) {
      uint8_t marker = data[i + 1];
      if (marker == 0xC0 || marker == 0xC2) {
        // SOF标记数据格式:
    // 2字节: 段长度
    // 1字节: 精度
    // 2字节: 高度
    // 2字节: 宽度
        if (i + 9 >= size) {
          return false;
        }
        height = (data[i + 5] << 8) | data[i + 6];
        width = (data[i + 7] << 8) | data[i + 8];
        return true;
      }
    }
  }
  return false;
}

// setCameraResolution函数的前向声明
bool setCameraResolution(int resolution);

// 提取完整的JPEG帧（从SOI到EOI）
static size_t trimToEOI(uint8_t* data, size_t size) {
  if (size < 2) {
    return 0;
  }

  size_t soiPos = 0;
  // 查找SOI标记
  for (; soiPos < size - 1; ++soiPos) {
    if (data[soiPos] == 0xFF && data[soiPos + 1] == 0xD8) {
      break;
    }
  }

  if (soiPos >= size - 1) {
    return 0; // SOI未找到
  }

  size_t eoiPos = soiPos + 2;
  // 查找EOI标记
  for (; eoiPos < size - 1; ++eoiPos) {
    if (data[eoiPos] == 0xFF && data[eoiPos + 1] == 0xD9) {
      break;
    }
  }

  if (eoiPos >= size - 1) {
    // EOI未找到，丢弃当前帧
    return 0;
  }

  // 返回有效的JPEG长度 (从SOI到EOI)
  return eoiPos - soiPos + 2;
}

// 通过相机快照接口获取并保存高清无边框JPEG
bool captureSnapshot() {
  // 稍等片刻以确保快照使用最新的capture_*参数
  delay(200);

  // 拍摄前停止MJPEG流以防止资源冲突
  logLine("Stopping MJPEG stream before capture...");
  
  // 使用全局变量直接停止流
  streamHttp.end();
  streamClient.stop();
  
  // 添加短暂延迟确保流完全停止
  delay(500);
  
  // 设置高分辨率（拍摄前）
  logLine("Setting high resolution before capture...");
  if (!setCameraResolution(CAMERA_RESOLUTION_HIGH)) {
    logLine("Failed to set high resolution");
    return false;
  }
  
  // 获取最新图像数据
  HTTPClient http;
  
  // 第一次请求：触发拍摄（忽略返回的旧图像数据）
  const char* captureUrl = "http://192.168.4.1/api/v1/capture";
  http.begin(captureUrl);
  // 使用简单的请求头，与Python代码保持一致
  http.addHeader("User-Agent", "M5Cardputer");
  http.setTimeout(15000);
  
  serialPrintf("[Snap] First request (trigger): GET %s\n", captureUrl);
  int code = http.GET();
  
  // 关闭第一次请求
  http.end();
  
  // 等待相机处理新图像
  logLine("Waiting for camera to process new image...");
  delay(500);
  
  // 第二次请求：获取新的图像数据
  http.begin(captureUrl);
  http.addHeader("User-Agent", "M5Cardputer");
  http.setTimeout(15000);
  logHttpRequestHeaders("snap", captureUrl, {{"User-Agent","M5Cardputer"}});
  serialPrintf("[Snap] Second request (fetch): GET %s\n", captureUrl);
  code = http.GET();
  logHttpResponseHeaders("snap", code, http);
  
  if (code != 200) {
    serialPrintf("[Snap] HTTP %d\n", code);
    logLine(String("[Snap] HTTP ") + code);
    http.end();
    return false;
  }
  String ct = http.header("Content-Type");
  serialPrintf("[Snap] CT: %s\n", ct.c_str());
  logLine(String("[Snap] CT=") + ct);
  
  // 验证内容类型是否为JPEG，但允许空内容类型（相机API可能不设置它）
  if (!ct.isEmpty() && !ct.startsWith("image/jpeg")) {
    serialPrintf("[Snap] Unexpected content-type: %s\n", ct.c_str());
    logLine(String("[Snap] Unexpected content-type: ") + ct);
    http.end();
    return false;
  }
  
  // 即使内容类型为空，由于状态为200，我们也会尝试将其作为JPEG处理
  
  WiFiClient* s = http.getStreamPtr();
  s->setNoDelay(true);
  s->setTimeout(10000); // 增加流读取超时时间
  int len = http.getSize(); // 如果未知则为-1
  logLine(String("[Snap] content-length=") + len);
  
  // 如果Content-Length过大，可能是错误
  if (len > 5 * 1024 * 1024) { // 限制最大5MB
    serialPrintf("[Snap] Content-Length too large: %d\n", len);
    logLine(String("[Snap] Content-Length too large: ") + len);
    http.end();
    return false;
  }
  
  // 使用静态数组代替栈分配大数组
  static uint8_t jpg[GLOBAL_MAX_JPEG_SIZE];
  size_t jpgSize = 0;
  
  // 将数据读入静态数组
  while (http.connected() && len && (jpgSize < GLOBAL_MAX_JPEG_SIZE)) {
    int available = s->available();
    if (available > 0) {
      int bytesRead = s->readBytes(jpg + jpgSize, min(available, (int)(GLOBAL_MAX_JPEG_SIZE - jpgSize)));
      jpgSize += bytesRead;
      if (len > 0) {
        len -= bytesRead;
      }
    }
  }
  
  // 检查是否读取了完整数据
  if (jpgSize >= GLOBAL_MAX_JPEG_SIZE) {
    serialPrintf("[Snap] JPEG data too large, truncated\n");
    logLine("[Snap] JPEG数据过大，已截断");
    http.end();
    return false;
  }
  
  // 提取完整的JPEG帧
  size_t validSize = trimToEOI(jpg, jpgSize);
  if (validSize == 0) {
    serialPrintf("[Snap] Invalid JPEG data, no complete frame\n");
    logLine("[Snap] 无效的JPEG数据，没有完整帧");
    http.end();
    return false;
  }
  
  // 保存到appState供以后使用
  memcpy(appState.jpegData, jpg, validSize);
  appState.jpegDataSize = validSize;
  
  // 验证JPEG尺寸
  int width, height;
  if (parseJpegSize(appState.jpegData, appState.jpegDataSize, width, height)) {
    serialPrintf("[Snap] JPEG size: %dx%d\n", width, height);
    logLine(String("[Snap] JPEG尺寸: ") + width + "x" + height);
  }
  
  http.end();
  
  // 设置低分辨率（拍摄后）
  logLine("Setting low resolution after capture...");
  if (!setCameraResolution(CAMERA_RESOLUTION_LOW)) {
    logLine("Failed to set low resolution");
    return false;
  }
  
  // 拍摄后重启MJPEG流
  logLine("Restarting MJPEG stream after capture...");
  appState.isRestartStream = true;
  
  return true;
}

// 处理MJPEG流
void processMjpegStream(WiFiClient& client) {
  static uint8_t jpegBuffer[GLOBAL_MAX_JPEG_SIZE];
  static size_t jpegIndex = 0;
  static bool inFrame = false;
  static uint8_t lastByte = 0;

  // 每次只处理一定数量的字节以避免阻塞
  const int MAX_BYTES_PER_CALL = 2048;
  int processed = 0;

  while (client.available() && processed < MAX_BYTES_PER_CALL) {
    uint8_t data = client.read();
    processed++;

    // SOI检测 (FF D8)
    if (!inFrame) {
      if (lastByte == 0xFF && data == 0xD8) {
        jpegIndex = 0;
        jpegBuffer[jpegIndex++] = 0xFF;
        jpegBuffer[jpegIndex++] = 0xD8;
        inFrame = true;
      }
      lastByte = data;
      continue;
    }

    // 帧内处理
    jpegBuffer[jpegIndex++] = data;

    // 如果帧太长则直接丢弃
    if (jpegIndex >= GLOBAL_MAX_JPEG_SIZE) {
      jpegIndex = 0;
      inFrame = false;
      lastByte = 0;
      continue;
    }

    // EOI检测 (FF D9)
    if (lastByte == 0xFF && data == 0xD9) {
      // 如果上一帧还未被消费则直接丢弃
      if (!appState.jpegReady) {
        memcpy(appState.jpegData, jpegBuffer, jpegIndex);
        appState.jpegDataSize = jpegIndex;
        appState.jpegReady = true;
      }
      jpegIndex = 0;
      inFrame = false;
    }

    lastByte = data;
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
  
  // 初始化SD卡
  M5Cardputer.Display.setCursor(10, 10);
  M5Cardputer.Display.println("Initializing SD card...");
  Serial.println("Initializing SD card...");
  
  // M5Cardputer SD卡引脚配置 - 使用与Python代码相同的引脚
  const int SD_CS = 12;
  const int SD_SCK = 40;
  const int SD_MISO = 39;
  const int SD_MOSI = 14;
  
  // 使用自定义SPI引脚初始化SD卡
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  
  if (!SD.begin(SD_CS)) {
    M5Cardputer.Display.setCursor(10, 25);
    M5Cardputer.Display.println("SD card initialization failed!");
    M5Cardputer.Display.setCursor(10, 40);
    M5Cardputer.Display.println("Press R to restart");
    Serial.println("SD card initialization failed!");
    Serial.println("Press R to restart");
    // 不再无限循环，允许用户按R键重启
    isSDInitialized = false;
    return;
  }
  
  isSDInitialized = true;
  M5Cardputer.Display.setCursor(10, 25);
  M5Cardputer.Display.println("SD card initialized successfully!");
  Serial.println("SD card initialized successfully!");
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
    logLine(String("[Res] HTTP ") + code);
    M5Cardputer.Display.println("Resolution setup failed!");
    http.end();
    return false;
  }
  
  http.end();
  serialPrintf("Camera resolution set to %d successfully\n", resolution);
  logLine("Camera resolution set successfully");
  M5Cardputer.Display.println("Camera resolution set!");
  return true;
}

// 初始化WiFi
bool initWiFi() {
  // 在屏幕上显示WiFi连接信息
  M5Cardputer.Display.setCursor(10, 40);
  M5Cardputer.Display.println("Connecting WiFi...");
  Serial.println("Connecting WiFi...");
  
  WiFi.begin("UnitCamS3-WiFi", "");
  int retry = 0;
  
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    delay(500);
    Serial.print(".");
    
    // 在屏幕上显示连接进度
    if (retry % 4 == 0) {
      M5Cardputer.Display.setCursor(10 + (retry / 4) * 10, 55);
      M5Cardputer.Display.print(".");
    }
    
    retry++;
  }
  
  M5Cardputer.Display.setCursor(10, 70);
  
  if (WiFi.status() != WL_CONNECTED) {
    logLine("WiFi connect failed");
    M5Cardputer.Display.println("WiFi connect failed!");
    return false;
  }
  
  String ipStr = WiFi.localIP().toString();
  logLine(String("WiFi connected: ") + ipStr);
  M5Cardputer.Display.println("WiFi connected!");
  M5Cardputer.Display.setCursor(10, 85);
  M5Cardputer.Display.println("IP: " + ipStr);
  
  // WiFi连接成功后设置相机分辨率（默认低分辨率）
  if (!setCameraResolution(CAMERA_RESOLUTION_LOW)) {
    logLine("Failed to set camera resolution");
    return false;
  }
  
  return true;
}

// 主循环
void loop() {
  M5Cardputer.update();
  
  // 处理用户按键
  if (M5Cardputer.Keyboard.isChange()) {
    M5Cardputer.Keyboard.updateKeysState();
    if (M5Cardputer.Keyboard.isKeyPressed('r')) {
      logLine("User requested device restart");
      M5Cardputer.Display.fillScreen(BLACK);
      M5Cardputer.Display.setCursor(10, 30);
      M5Cardputer.Display.setTextSize(2);
      M5Cardputer.Display.println("Restarting device...");
      delay(1000);
      ESP.restart();
    }
  }
  
  // 处理BtnA按下（拍照）
  if (M5Cardputer.BtnA.wasPressed()) {
    appState.isCaptureReq = true;
  }
  
  // 处理拍摄请求
  if (appState.isCaptureReq) {
    appState.isCaptureReq = false;
    logLine("Processing capture request...");
    if (captureSnapshot()) {
      logLine("Capture successful");
      
      // 保存照片到SD卡
      if (isSDInitialized) {
        logLine("Saving photo to SD card...");
        
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
          logLine("Failed to open file");
        } else {
          // 写入JPEG数据
          size_t bytesWritten = file.write(appState.jpegData, appState.jpegDataSize);
          if (bytesWritten != appState.jpegDataSize) {
            logLine("Failed to write to file");
          } else {
            logLine(String("Photo saved successfully: ") + filename);
            M5Cardputer.Display.setCursor(10, 10);
            M5Cardputer.Display.println(String("Photo saved: ") + filename);
          }
          file.close();
        }
      } else {
        logLine("SD card not initialized, cannot save photo");
        M5Cardputer.Display.setCursor(10, 10);
        M5Cardputer.Display.println("SD card not initialized");
      }
    } else {
      logLine("Capture failed");
    }
  }
  
  // 检查WiFi连接状态
  if (WiFi.status() == WL_CONNECTED) {
    if (!streamClient.connected()) {
      if (appState.isRestartStream || !streamHttp.connected()) {
        appState.isRestartStream = false;
        streamHttp.end();
        streamClient.stop();
        
        logLine("Connecting to MJPEG stream...");
        String url = "http://192.168.4.1/api/v1/stream";
        streamHttp.begin(streamClient, url);
        streamHttp.addHeader("User-Agent", "M5Cardputer");
        streamHttp.addHeader("Connection", "keep-alive");
        
        int code = streamHttp.GET();
        if (code != 200) {
          logLine(String("Failed to connect to MJPEG stream: HTTP ") + code);
          streamHttp.end();
          delay(2000);
          return;
        }
        
        logLine("MJPEG stream connected successfully");
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
      logLine("WiFi disconnected, waiting for network recovery...");
    }
  }
  
  // 显示JPEG帧
  if (appState.jpegReady) {
    // 获取JPEG尺寸
    int imgWidth, imgHeight;
    if (parseJpegSize(appState.jpegData, appState.jpegDataSize, imgWidth, imgHeight)) {
      // M5Cardputer屏幕分辨率
      const int screenWidth = 240;
      const int screenHeight = 135;
      
      // 计算显示起始位置（居中显示）
      int x = 0;
      int y = 0;
      
      // 如果图像宽度大于屏幕宽度，只显示中间部分
      if (imgWidth > screenWidth) {
        x = -(imgWidth - screenWidth) / 2;
      }
      
      // 如果图像高度大于屏幕高度，只显示中间部分
      if (imgHeight > screenHeight) {
        y = -(imgHeight - screenHeight) / 2;
      }
      
      // 向LCD显示JPEG帧
      M5Cardputer.Display.drawJpg(appState.jpegData, appState.jpegDataSize, x, y);
    } else {
      // 如果无法解析尺寸，默认显示左上角
      M5Cardputer.Display.drawJpg(appState.jpegData, appState.jpegDataSize, 0, 0);
    }
    
    // 显示后重置就绪标志
    appState.jpegReady = false;
  }
  
  delay(10);
}

// 主函数
void setup() {
  // 显示应用标题
  M5Cardputer.Display.setCursor(10, 10);
  M5Cardputer.Display.setTextSize(2);
  M5Cardputer.Display.println("Camera App");
  M5Cardputer.Display.setTextSize(1);
  
  initHardware();
  
  if (!initWiFi()) {
    logLine("WiFi initialization failed");
    M5Cardputer.Display.setCursor(10, 30);
    M5Cardputer.Display.setTextSize(2);
    M5Cardputer.Display.println("WiFi connect failed");
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.println("please check network config");
    M5Cardputer.Display.println("press R to restart");
  } else {
    logLine("Camera application initialized successfully");
    
    // 显示初始化完成信息
    M5Cardputer.Display.setCursor(10, 130);
    M5Cardputer.Display.println("Initialization completed!");
    
    // 短暂显示提示信息
    delay(1000);
    
    // 清屏，准备显示流画面
    M5Cardputer.Display.fillScreen(BLACK);
    
    // 显示操作提示
    M5Cardputer.Display.setCursor(10, 5);
    M5Cardputer.Display.setTextColor(WHITE);
    M5Cardputer.Display.println("Press BtnA to capture");
  }
}