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
#define GLOBAL_MAX_JPEG_SIZE 70 * 1024 // 70KB最大JPEG尺寸，进一步减小以节省内存

// 相机分辨率常量
#define CAMERA_RESOLUTION_HIGH 13     // 13高分辨率 (1280*720)，用于拍摄照片
#define CAMERA_RESOLUTION_TIMELAPSE 10     // 10分辨率 (640*480)，用于延时摄影模式
#define CAMERA_RESOLUTION_LOW 6       // 6低分辨率(320*240)，用于实时预览

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
  
  // 缓存的图像尺寸（避免每帧都解析）
  int cachedImgWidth;
  int cachedImgHeight;
  bool sizeCached;
} AppState;

AppState appState = {
  false,                   // isCaptureReq
  false,                   // isRestartStream
  false,                   // jpegReady
  {0},                     // jpegData
  0,                       // jpegDataSize
  0,                       // cachedImgWidth
  0,                       // cachedImgHeight
  false                    // sizeCached
};

// 屏幕分辨率常量定义
const int SCREEN_WIDTH = 240;
const int SCREEN_HEIGHT = 135;

// SD卡状态全局变量
bool isSDInitialized = false;

// 相机参数状态全局变量
int currentBrightness = 0;
int currentContrast = 0;
int currentSaturation = 0;
int currentSharpness = 0;

// 屏幕显示状态
int currentDisplayLine = 0;
bool isShowingStatus = false;
int statusScrollOffset = 0;

// 按键防抖动变量
unsigned long lastKeyPressTime = 0;
const unsigned long keyDebounceDelay = 200; // 按键防抖动延迟200ms

// Timelapse延时摄影模式相关变量
bool isTimelapseMode = false;        // 是否处于timelapse模式
int timelapsePhotoCount = 0;         // 已拍摄照片数量
int currentTimelapseSession = 0;     // 当前timelapse会话编号
unsigned long timelapseLastShotTime = 0; // 上次拍摄时间
const unsigned long timelapseInterval = 5000; // 拍摄间隔5秒
unsigned long timelapseStartTime = 0; // timelapse模式启动时间
bool isScreenOff = false;             // 屏幕是否息屏
unsigned long lastUserActionTime = 0; // 上次用户操作时间
const unsigned long screenOffTimeout = 60000; // 1分钟无操作息屏
String currentTimelapseDir = "";      // 当前timelapse会话的目录路径

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
  // logLine(logStr);
  for (const auto& header : headers) {
    logStr = String("[") + prefix + "] Header: " + header.first + ": " + header.second;
    // logLine(logStr);
  }
}

// 记录HTTP响应头
void logHttpResponseHeaders(const String& prefix, int code, HTTPClient& http) {
  String logStr = String("[") + prefix + "] Response: HTTP " + code;
  // logLine(logStr);
  for (const char* key : LOG_HDR_KEYS) {
    String value = http.header(key);
    if (value.length() > 0) {
      logStr = String("[") + prefix + "] Header: " + key + ": " + value;
      // logLine(logStr);
    }
  }
}

// 记录原始串口数据
void serialPrintf(const char* format, ...) {
  return;
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

// setCameraQuality函数的前向声明
bool setCameraQuality(int quality);

// setCameraSpecialEffect函数的前向声明
bool setCameraSpecialEffect(int effect);

// getCameraConfig函数的前向声明
bool getCameraConfig();

// setCameraParameter函数的前向声明
bool setCameraParameter(const String& paramName, int value);

// displayLine函数的前向声明
void displayLine(const String& text);

// showStatusFile函数的前向声明
void showStatusFile();

// loadCameraStatus函数的前向声明
bool loadCameraStatus();

// createTimelapseDir函数的前向声明
bool createTimelapseDir();

// getSDCardFreeSpace函数的前向声明
uint64_t getSDCardFreeSpace();

// getBatteryPercentage函数的前向声明
int getBatteryPercentage();

// updateTimelapseDisplay函数的前向声明
void updateTimelapseDisplay();

// startTimelapseMode函数的前向声明
void startTimelapseMode();

// stopTimelapseMode函数的前向声明
void stopTimelapseMode();

// captureTimelapsePhoto函数的前向声明
bool captureTimelapsePhoto();

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
  delay(500);

  // 拍摄前停止MJPEG流以防止资源冲突
  // logLine("Stopping MJPEG stream before capture...");
  
  // 使用全局变量直接停止流
  streamHttp.end();
  streamClient.stop();
  
  // 添加短暂延迟确保流完全停止
  delay(500);
  
  // 设置高分辨率（拍摄前）
  // logLine("Setting high resolution before capture...");
  if (!setCameraResolution(CAMERA_RESOLUTION_HIGH)) {
    // logLine("Failed to set high resolution");
    return false;
  }
  
  // 设置高质量（拍摄前）
  if (!setCameraQuality(2)) {
    // logLine("Failed to set high quality");
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
  // logLine("Waiting for camera to process new image...");
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
    // logLine(String("[Snap] HTTP ") + code);
    http.end();
    return false;
  }
  String ct = http.header("Content-Type");
  serialPrintf("[Snap] CT: %s\n", ct.c_str());
  // logLine(String("[Snap] CT=") + ct);
  
  // 验证内容类型是否为JPEG，但允许空内容类型（相机API可能不设置它）
  if (!ct.isEmpty() && !ct.startsWith("image/jpeg")) {
    serialPrintf("[Snap] Unexpected content-type: %s\n", ct.c_str());
    // logLine(String("[Snap] Unexpected content-type: ") + ct);
    http.end();
    return false;
  }
  
  // 即使内容类型为空，由于状态为200，我们也会尝试将其作为JPEG处理
  
  WiFiClient* s = http.getStreamPtr();
  s->setNoDelay(true);
  s->setTimeout(10000); // 增加流读取超时时间
  int len = http.getSize(); // 如果未知则为-1
  // logLine(String("[Snap] content-length=") + len);
  
  // 如果Content-Length过大，可能是错误
  if (len > 5 * 1024 * 1024) { // 限制最大5MB
    serialPrintf("[Snap] Content-Length too large: %d\n", len);
    // logLine(String("[Snap] Content-Length too large: ") + len);
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
    // logLine("[Snap] JPEG数据过大，已截断");
    http.end();
    return false;
  }
  
  // 提取完整的JPEG帧
  size_t validSize = trimToEOI(jpg, jpgSize);
  if (validSize == 0) {
    serialPrintf("[Snap] Invalid JPEG data, no complete frame\n");
    // logLine("[Snap] 无效的JPEG数据，没有完整帧");
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
    // logLine(String("[Snap] JPEG尺寸: ") + width + "x" + height);
  }
  
  http.end();
  
  // 设置低分辨率（拍摄后）
  // logLine("Setting low resolution after capture...");
  if (!setCameraResolution(CAMERA_RESOLUTION_LOW)) {
    // logLine("Failed to set low resolution");
    return false;
  }
  
  // 设置低质量（拍摄后，恢复串流模式）
  if (!setCameraQuality(0)) {
    // logLine("Failed to set low quality");
    return false;
  }
  
  // 拍摄后重启MJPEG流
  // logLine("Restarting MJPEG stream after capture...");
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
    if (lastByte == 0xFF && data == 0xD8) {
      // 检测到SOI，无条件清空所有状态，确保jpegBuffer从SOI开始
      jpegIndex = 0;
      jpegBuffer[jpegIndex++] = 0xFF;
      jpegBuffer[jpegIndex++] = 0xD8;
      inFrame = true;
    } else if (inFrame) {
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
  
  // 获取响应数据长度
  int contentLength = http.getSize();
  serialPrintf("Content length: %d bytes\n", contentLength);
  
  // 获取响应数据
  String statusData = http.getString();
  http.end();
  
  // 调试：打印从摄像头获取的原始状态数据
  Serial.printf("Raw status data length: %d\n", statusData.length());
  Serial.printf("Raw status data from camera: %s\n", statusData.c_str());
  
  // 检查是否真的从摄像头获取了新数据
  if (statusData.length() == 0) {
    serialPrintf("ERROR: No data received from camera!\n");
    displayLine("No data received!");
    return false;
  }
  
  // 检查SD卡是否已初始化
  if (!isSDInitialized) {
    serialPrintf("SD card not initialized, cannot save status\n");
    displayLine("SD not initialized!");
    return false;
  }
  
  // 创建/images目录（如果不存在）
  if (!SD.exists("/images")) {
    SD.mkdir("/images");
  }
  
  // 保存配置到SD卡
  File statusFile = SD.open("/images/status.txt", FILE_WRITE);
  if (!statusFile) {
    serialPrintf("Failed to open status file\n");
    displayLine("Failed to open file!");
    return false;
  }
  
  size_t bytesWritten = statusFile.print(statusData);
  statusFile.close();
  
  if (bytesWritten != statusData.length()) {
    serialPrintf("Failed to write status data\n");
    displayLine("Failed to write!");
    return false;
  }
  
  serialPrintf("Camera status saved to /images/status.txt\n");
  displayLine("Config saved!");
  
  // 加载状态到全局变量
  loadCameraStatus();
  
  return true;
}

// 从SD卡加载相机状态并更新全局变量
bool loadCameraStatus() {
  // 检查SD卡是否已初始化
  if (!isSDInitialized) {
    serialPrintf("SD card not initialized, cannot load status\n");
    return false;
  }
  
  // 读取status.txt文件
  File statusFile = SD.open("/images/status.txt", FILE_READ);
  if (!statusFile) {
    serialPrintf("Failed to open status.txt for reading\n");
    return false;
  }
  
  String statusData = statusFile.readString();
  statusFile.close();
  
  if (statusData.length() == 0) {
    serialPrintf("status.txt is empty\n");
    return false;
  }
  
  // 解析JSON数据，提取参数值
  // 格式: {"framesize":6,"quality":0,"brightness":0,"contrast":0,"saturation":0,"sharpness":0,...}
  
  int brightnessPos = statusData.indexOf("\"brightness\":");
  if (brightnessPos != -1) {
    int valueStart = brightnessPos + 13; // 跳过"brightness":
    int valueEnd = statusData.indexOf(',', valueStart);
    if (valueEnd == -1) {
      valueEnd = statusData.indexOf('}', valueStart);
    }
    if (valueEnd != -1) {
      String valueStr = statusData.substring(valueStart, valueEnd);
      currentBrightness = valueStr.toInt();
      serialPrintf("Loaded brightness: %d\n", currentBrightness);
    }
  }
  
  int contrastPos = statusData.indexOf("\"contrast\":");
  if (contrastPos != -1) {
    int valueStart = contrastPos + 11; // 跳过"contrast":
    int valueEnd = statusData.indexOf(',', valueStart);
    if (valueEnd == -1) {
      valueEnd = statusData.indexOf('}', valueStart);
    }
    if (valueEnd != -1) {
      String valueStr = statusData.substring(valueStart, valueEnd);
      currentContrast = valueStr.toInt();
      serialPrintf("Loaded contrast: %d\n", currentContrast);
    }
  }
  
  int saturationPos = statusData.indexOf("\"saturation\":");
  if (saturationPos != -1) {
    int valueStart = saturationPos + 14; // 跳过"saturation":
    int valueEnd = statusData.indexOf(',', valueStart);
    if (valueEnd == -1) {
      valueEnd = statusData.indexOf('}', valueStart);
    }
    if (valueEnd != -1) {
      String valueStr = statusData.substring(valueStart, valueEnd);
      currentSaturation = valueStr.toInt();
      serialPrintf("Loaded saturation: %d\n", currentSaturation);
    }
  }
  
  int sharpnessPos = statusData.indexOf("\"sharpness\":");
  if (sharpnessPos != -1) {
    int valueStart = sharpnessPos + 12; // 跳过"sharpness":
    int valueEnd = statusData.indexOf(',', valueStart);
    if (valueEnd == -1) {
      valueEnd = statusData.indexOf('}', valueStart);
    }
    if (valueEnd != -1) {
      String valueStr = statusData.substring(valueStart, valueEnd);
      currentSharpness = valueStr.toInt();
      serialPrintf("Loaded sharpness: %d\n", currentSharpness);
    }
  }
  
  serialPrintf("Camera status loaded successfully\n");
  return true;
}

// 创建timelapse目录
bool createTimelapseDir() {
  if (!isSDInitialized) {
    serialPrintf("SD card not initialized, cannot create timelapse directory\n");
    return false;
  }
  
  // 创建/images/timelapse主目录
  if (!SD.exists("/images/timelapse")) {
    if (!SD.mkdir("/images/timelapse")) {
      serialPrintf("Failed to create /images/timelapse directory\n");
      return false;
    }
  }
  
  // 查找最大的会话编号
  int maxSession = -1;
  File root = SD.open("/images/timelapse");
  if (root) {
    File file = root.openNextFile();
    while (file) {
      if (!file.isDirectory()) {
        file = root.openNextFile();
        continue;
      }
      
      String dirName = file.name();
      // 提取数字编号
      int sessionNum = dirName.toInt();
      if (sessionNum > maxSession) {
        maxSession = sessionNum;
      }
      
      file = root.openNextFile();
    }
    root.close();
  }
  
  // 新会话编号为最大编号+1，如果为空则为0
  currentTimelapseSession = maxSession + 1;
  
  // 生成子目录名称
  char dirName[32];
  snprintf(dirName, sizeof(dirName), "/images/timelapse/%d", currentTimelapseSession);
  
  currentTimelapseDir = String(dirName);
  
  // 创建子目录
  if (!SD.mkdir(currentTimelapseDir)) {
    serialPrintf("Failed to create %s directory\n", currentTimelapseDir.c_str());
    return false;
  }
  
  serialPrintf("Created timelapse directory: %s\n", currentTimelapseDir.c_str());
  return true;
}

// 获取SD卡剩余容量（字节）
uint64_t getSDCardFreeSpace() {
  if (!isSDInitialized) {
    return 0;
  }
  
  uint64_t totalBytes = SD.totalBytes();
  uint64_t usedBytes = SD.usedBytes();
  uint64_t freeBytes = totalBytes - usedBytes;
  
  return freeBytes;
}

// 获取电池电量百分比
int getBatteryPercentage() {
  M5Cardputer.update();
  
  float voltage = M5Cardputer.Power.getBatteryVoltage();
  
  // 假设电池电压范围：3.0V（0%）到4.2V（100%）
  float percentage = (voltage - 3.0f) / (4.2f - 3.0f) * 100.0f;
  
  if (percentage < 0) percentage = 0;
  if (percentage > 100) percentage = 100;
  
  return (int)percentage;
}

// 更新timelapse模式显示界面
void updateTimelapseDisplay() {
  if (isScreenOff) {
    return;
  }
  
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5Cardputer.Display.setTextSize(1);
  
  // 左上角：已拍摄张数和倒计时
  M5Cardputer.Display.setCursor(5, 5);
  M5Cardputer.Display.printf("Photos: %d", timelapsePhotoCount);
  
  unsigned long timeSinceLastShot = millis() - timelapseLastShotTime;
  int countdown;
  
  if (timeSinceLastShot >= timelapseInterval) {
    countdown = 0;
  } else {
    countdown = (timelapseInterval - timeSinceLastShot) / 1000;
  }
  
  M5Cardputer.Display.setCursor(5, 20);
  if (timeSinceLastShot >= timelapseInterval) {
    M5Cardputer.Display.fillRect(5, 20, 100, 10, TFT_BLACK);
    M5Cardputer.Display.printf("Capturing");
  } else {
    M5Cardputer.Display.fillRect(5, 20, 100, 10, TFT_BLACK);
    M5Cardputer.Display.printf("Next: %ds", countdown);
  }
  
  // 右上角：存储卡剩余容量和电量百分比
  uint64_t freeSpace = getSDCardFreeSpace();
  float freeSpaceMB = freeSpace / (1024.0f * 1024.0f);
  int battery = getBatteryPercentage();
  
  String spaceStr = String(freeSpaceMB, 1) + "MB";
  int spaceStrWidth = M5Cardputer.Display.textWidth(spaceStr);
  M5Cardputer.Display.setCursor(SCREEN_WIDTH - spaceStrWidth - 5, 5);
  M5Cardputer.Display.printf("%s", spaceStr.c_str());
  
  String batteryStr = String(battery) + "%";
  int batteryStrWidth = M5Cardputer.Display.textWidth(batteryStr);
  M5Cardputer.Display.setCursor(SCREEN_WIDTH - batteryStrWidth - 5, 20);
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
  
  serialPrintf("Restoring low quality...\n");
  setCameraQuality(0);
  
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
  
  // 查找当前会话中最大的照片编号
  int maxPhotoNum = -1;
  File sessionDir = SD.open(currentTimelapseDir);
  if (sessionDir) {
    File file = sessionDir.openNextFile();
    while (file) {
      if (file.isDirectory()) {
        file = sessionDir.openNextFile();
        continue;
      }
      
      String fileName = file.name();
      // 文件名格式：IMG_XXXX_YYYY.jpg，提取YYYY作为编号
      int lastSlash = fileName.lastIndexOf('/');
      if (lastSlash >= 0) {
        fileName = fileName.substring(lastSlash + 1);
      }
      
      if (fileName.startsWith("IMG_") && fileName.endsWith(".jpg")) {
        // 找到第二个下划线的位置
        int firstUnderscore = fileName.indexOf('_');
        int secondUnderscore = fileName.indexOf('_', firstUnderscore + 1);
        
        if (secondUnderscore > 0) {
          String numStr = fileName.substring(secondUnderscore + 1, fileName.length() - 4);
          int photoNum = numStr.toInt();
          if (photoNum > maxPhotoNum) {
            maxPhotoNum = photoNum;
          }
        }
      }
      
      file = sessionDir.openNextFile();
    }
    sessionDir.close();
  }
  
  // 新照片编号为最大编号+1
  int photoNum = maxPhotoNum + 1;
  
  // 生成文件名：IMG_XXXX_YYYY.jpg
  char filename[64];
  snprintf(filename, sizeof(filename), "%s/IMG_%d_%04d.jpg",
           currentTimelapseDir.c_str(), currentTimelapseSession, photoNum);
  
  // 保存照片到SD卡
  File photoFile = SD.open(filename, FILE_WRITE);
  if (!photoFile) {
    serialPrintf("[Timelapse] Failed to create photo file\n");
    http.end();
    // 即使文件创建失败，也要重置倒计时，避免卡在0秒
    timelapseLastShotTime = millis();
    return false;
  }
  
  uint8_t* buffer = new uint8_t[1024];
  size_t totalWritten = 0;
  
  while (http.connected() && (len < 0 || totalWritten < (size_t)len)) {
    size_t available = s->available();
    if (available > 0) {
      int readSize = s->readBytes(buffer, min(available, (size_t)1024));
      photoFile.write(buffer, readSize);
      totalWritten += readSize;
    }
  }
  
  delete[] buffer;
  photoFile.close();
  http.end();
  
  serialPrintf("[Timelapse] Written: %d bytes\n", totalWritten);
  
  if (len > 0 && totalWritten != (size_t)len) {
    serialPrintf("[Timelapse] Incomplete photo: %d/%d bytes\n", totalWritten, len);
    // 即使数据不完整，也要重置倒计时，避免卡在0秒
    timelapseLastShotTime = millis();
    return false;
  }
  
  timelapsePhotoCount++;
  timelapseLastShotTime = millis();
  
  serialPrintf("[Timelapse] Photo saved: %s\n", filename);
  serialPrintf("[Timelapse] Total photos: %d\n", timelapsePhotoCount);
  serialPrintf("[Timelapse] Next photo in 5 seconds\n");
  
  return true;
}

// 通用的设置相机参数函数
bool setCameraParameter(const String& paramName, int value) {
  // 在屏幕上显示参数设置信息
  M5Cardputer.Display.setCursor(10, 205);
  M5Cardputer.Display.printf("Setting %s to %d...\n", paramName.c_str(), value);
  Serial.printf("Setting %s to %d...\n", paramName.c_str(), value);
  
  HTTPClient http;
  String url = String("http://192.168.4.1/api/v1/control?var=") + paramName + "&val=" + value;
  
  http.begin(url);
  http.addHeader("User-Agent", "M5Cardputer");
  http.setTimeout(10000);
  
  int code = http.GET();
  logHttpResponseHeaders("param", code, http);
  
  // 调试：打印HTTP响应内容长度和内容
  int contentLength = http.getSize();
  Serial.printf("HTTP Content length for %s: %d bytes\n", paramName.c_str(), contentLength);
  
  String response = http.getString();
  Serial.printf("HTTP Response length for %s: %d bytes\n", paramName.c_str(), response.length());
  Serial.printf("HTTP Response for %s: %s\n", paramName.c_str(), response.c_str());
  
  M5Cardputer.Display.setCursor(10, 220);
  
  if (code != 200) {
    serialPrintf("[%s] HTTP %d\n", paramName.c_str(), code);
    M5Cardputer.Display.println("Param setup failed!");
    http.end();
    return false;
  }
  
  http.end();
  serialPrintf("%s set to %d successfully\n", paramName.c_str(), value);
  M5Cardputer.Display.println("Param set!");
  
  // 更新对应的全局变量
  if (paramName == "brightness") {
    currentBrightness = value;
    Serial.printf("Updated currentBrightness to %d\n", currentBrightness);
  } else if (paramName == "contrast") {
    currentContrast = value;
    Serial.printf("Updated currentContrast to %d\n", currentContrast);
  } else if (paramName == "saturation") {
    currentSaturation = value;
    Serial.printf("Updated currentSaturation to %d\n", currentSaturation);
  } else if (paramName == "sharpness") {
    currentSharpness = value;
    Serial.printf("Updated currentSharpness to %d\n", currentSharpness);
  }
  
  return true;
}

// 显示文本行（支持滚动）
void displayLine(const String& text) {
  int lineHeight = 12;
  int maxLines = 20;
  
  if (currentDisplayLine >= maxLines) {
    // 清空屏幕并重置
    M5Cardputer.Display.fillScreen(BLACK);
    currentDisplayLine = 0;
  }
  
  M5Cardputer.Display.setCursor(10, 10 + currentDisplayLine * lineHeight);
  M5Cardputer.Display.println(text);
  currentDisplayLine++;
}

// 显示status.txt内容
void showStatusFile() {
  // 先重新获取最新配置并保存到SD卡
  getCameraConfig();
  
  // 从SD卡加载相机状态并更新全局变量
  loadCameraStatus();
  
  if (!isSDInitialized) {
    M5Cardputer.Display.fillScreen(BLACK);
    M5Cardputer.Display.setCursor(10, 10);
    M5Cardputer.Display.println("SD card not initialized!");
    delay(2000);
    return;
  }
  
  // 停止视频流
  if (streamClient.connected()) {
    streamClient.stop();
  }
  if (streamHttp.connected()) {
    streamHttp.end();
  }
  
  File statusFile = SD.open("/images/status.txt", FILE_READ);
  if (!statusFile) {
    M5Cardputer.Display.fillScreen(BLACK);
    M5Cardputer.Display.setCursor(10, 10);
    M5Cardputer.Display.println("Failed to open status.txt!");
    delay(2000);
    return;
  }
  
  // 读取文件内容
  String statusData = statusFile.readString();
  statusFile.close();
  
  // 解析JSON数据
  statusData.replace("{", "");
  statusData.replace("}", "");
  statusData.replace("\"", "");
  
  // 按逗号分割
  int start = 0;
  int end = statusData.indexOf(',');
  String lines[30];
  int lineCount = 0;
  
  while (end != -1 && lineCount < 30) {
    lines[lineCount] = statusData.substring(start, end);
    lineCount++;
    start = end + 1;
    end = statusData.indexOf(',', start);
  }
  
  // 添加最后一个参数
  if (start < statusData.length() && lineCount < 30) {
    lines[lineCount] = statusData.substring(start);
    lineCount++;
  }
  
  // 添加按键提示
  String keyHints[10];
  int hintCount = 0;
  
  for (int i = 0; i < lineCount; i++) {
    String param = lines[i];
    int colonPos = param.indexOf(':');
    if (colonPos != -1) {
      String paramName = param.substring(0, colonPos);
      String paramValue = param.substring(colonPos + 1);
      
      String hint = "";
      if (paramName == "brightness") {
        hint = " [;/.]";
      } else if (paramName == "contrast") {
        hint = " [,/]";
      } else if (paramName == "saturation") {
        hint = " [[]]";
      } else if (paramName == "sharpness") {
        hint = " [_/=]";
      } else if (paramName == "special_effect") {
        hint = " [0-6]";
      }
      
      if (hint.length() > 0) {
        keyHints[hintCount] = paramName + ": " + paramValue + hint;
        hintCount++;
      }
    }
  }
  
  // 显示状态信息
  isShowingStatus = true;
  statusScrollOffset = 0;
  
  while (isShowingStatus) {
    M5Cardputer.update();
    M5Cardputer.Display.fillScreen(BLACK);
    
    int lineHeight = 12;
    int maxLines = 20;
    int displayLine = 0;
    
    // 显示标题
    M5Cardputer.Display.setCursor(10, 10);
    M5Cardputer.Display.println("Camera Status (ESC to exit)");
    displayLine++;
    
    // 显示参数
    for (int i = statusScrollOffset; i < lineCount && displayLine < maxLines; i++) {
      M5Cardputer.Display.setCursor(10, 10 + displayLine * lineHeight);
      M5Cardputer.Display.println(lines[i]);
      displayLine++;
    }
    
    // 显示按键提示
    displayLine++;
    M5Cardputer.Display.setCursor(10, 10 + displayLine * lineHeight);
    M5Cardputer.Display.println("--- Key Hints ---");
    displayLine++;
    
    for (int i = 0; i < hintCount && displayLine < maxLines; i++) {
      M5Cardputer.Display.setCursor(10, 10 + displayLine * lineHeight);
      M5Cardputer.Display.println(keyHints[i]);
      displayLine++;
    }
    
    // 显示滚动提示
    if (lineCount > maxLines - 5) {
      M5Cardputer.Display.setCursor(10, 10 + (maxLines - 1) * lineHeight);
      M5Cardputer.Display.printf("Use UP/DOWN to scroll (%d/%d)", statusScrollOffset + 1, lineCount - maxLines + 6);
    }
    
    delay(100);
    
    // 检查按键
    if (M5Cardputer.Keyboard.isChange()) {
      M5Cardputer.Keyboard.updateKeysState();
      
      // ESC键退出
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
        if (statusScrollOffset < lineCount - maxLines + 6) {
          statusScrollOffset++;
        }
      }
    }
  }
  
  // 退出后清空屏幕并重启视频流
  M5Cardputer.Display.fillScreen(BLACK);
  currentDisplayLine = 0;
  
  // 标记需要重启视频流
  appState.isRestartStream = true;
}

// 初始化WiFi
bool initWiFi() {
  // 在屏幕上显示WiFi连接信息
  displayLine("Connecting WiFi...");
  Serial.println("Connecting WiFi...");
  
  WiFi.begin("UnitCamS3-WiFi", "");
  int retry = 0;
  
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    delay(500);
    Serial.print(".");
    
    // 在屏幕上显示连接进度
    if (retry % 4 == 0) {
      M5Cardputer.Display.setCursor(10 + (retry / 4) * 10, 10 + currentDisplayLine * 12);
      M5Cardputer.Display.print(".");
    }
    
    retry++;
  }
  
  currentDisplayLine++;
  
  if (WiFi.status() != WL_CONNECTED) {
    // logLine("WiFi connect failed");
    displayLine("WiFi connect failed!");
    return false;
  }
  
  String ipStr = WiFi.localIP().toString();
  // logLine(String("WiFi connected: ") + ipStr);
  displayLine("WiFi connected!");
  displayLine("IP: " + ipStr);
  
  // WiFi连接成功后设置相机分辨率（默认低分辨率）
  if (!setCameraResolution(CAMERA_RESOLUTION_LOW)) {
    // logLine("Failed to set camera resolution");
    return false;
  }
  
  // 设置相机质量为0（串流模式）
  if (!setCameraQuality(0)) {
    // logLine("Failed to set camera quality");
    return false;
  }
  
  // 获取相机配置并保存到SD卡
  getCameraConfig();
  
  // 从SD卡加载相机状态并更新全局变量
  loadCameraStatus();
  
  return true;
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
          size_t bytesWritten = file.write(appState.jpegData, appState.jpegDataSize);
          if (bytesWritten != appState.jpegDataSize) {
            // logLine("Failed to write to file");
          } else {
            // logLine(String("Photo saved successfully: ") + filename);
            M5Cardputer.Display.setCursor(10, 10);
            M5Cardputer.Display.println(String("Photo saved: ") + filename);
          }
          file.close();
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
      if (appState.isRestartStream || !streamHttp.connected()) {
        appState.isRestartStream = false;
        
        // 清除图像尺寸缓存（因为流重启了）
        appState.sizeCached = false;
        appState.cachedImgWidth = 0;
        appState.cachedImgHeight = 0;
        
        streamHttp.end();
        streamClient.stop();
        
        // 等待相机完成分辨率切换
        delay(500);
        
        // logLine("Connecting to MJPEG stream...");
        String url = "http://192.168.4.1/api/v1/stream";
        streamHttp.begin(streamClient, url);
        streamHttp.addHeader("User-Agent", "M5Cardputer");
        streamHttp.addHeader("Connection", "keep-alive");
        
        int code = streamHttp.GET();
        if (code != 200) {
          // logLine(String("Failed to connect to MJPEG stream: HTTP ") + code);
          streamHttp.end();
          delay(2000);
          return;
        }
        
        // logLine("MJPEG stream connected successfully");
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
  
  // 显示JPEG帧
  if (appState.jpegReady) {
    int imgWidth, imgHeight;
    
    // 尝试使用缓存的图像尺寸
    if (appState.sizeCached) {
      imgWidth = appState.cachedImgWidth;
      imgHeight = appState.cachedImgHeight;
    } else {
      // 解析JPEG尺寸
      if (parseJpegSize(appState.jpegData, appState.jpegDataSize, imgWidth, imgHeight)) {
        // 缓存图像尺寸
        appState.cachedImgWidth = imgWidth;
        appState.cachedImgHeight = imgHeight;
        appState.sizeCached = true;
      } else {
        // 如果无法解析尺寸，默认显示左上角
        M5Cardputer.Display.drawJpg(appState.jpegData, appState.jpegDataSize, 0, 0);
        appState.jpegReady = false;
        return;
      }
    }
    
    // 计算显示起始位置（居中显示）
    int x = 0;
    int y = 0;
    
    // 如果图像宽度小于屏幕宽度，居中显示
    if (imgWidth < SCREEN_WIDTH) {
      x = (SCREEN_WIDTH - imgWidth) / 2;
    }
    
    // 如果图像高度小于屏幕高度，居中显示
    if (imgHeight < SCREEN_HEIGHT) {
      y = (SCREEN_HEIGHT - imgHeight) / 2;
    }
    
    // 向LCD显示JPEG帧
    // 注意：如果图像大于屏幕，drawJpg会自动裁切显示左上角部分
    M5Cardputer.Display.drawJpg(appState.jpegData, appState.jpegDataSize, x, y);
    
    // 显示后重置就绪标志
    appState.jpegReady = false;
  }
  
  //delay(10);
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
    // logLine("WiFi initialization failed");
    M5Cardputer.Display.setCursor(10, 30);
    M5Cardputer.Display.setTextSize(2);
    M5Cardputer.Display.println("WiFi connect failed");
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.println("please check network status");
    M5Cardputer.Display.println("press R to restart");
  } else {
    // logLine("Camera application initialized successfully");
    
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