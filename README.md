# M5Cardputer Camera Application
# M5Cardputer 相机应用

## Overview
## 项目概述

A simple camera application for M5Cardputer, developed with PlatformIO, that enables MJPEG streaming and image capture with SD card storage.

这是一个为M5Cardputer开发的简单相机应用，基于PlatformIO开发，支持MJPEG串流和带有SD卡存储功能的图像捕获。
![cardputer_Camera](review.jpg)
## Features
## 功能特性

- **MJPEG Streaming**: Real-time camera feed display
- **Image Capture**: High-quality photo taking
- **WiFi File Transfer**: Access photos via browser over hotspot
- **Software Filters**: Creative effects like GameBoy, Pixelate, Vintage Film via LUT
- **SD Card Storage**: Save captured images to storage

- **MJPEG串流**：实时相机画面显示
- **图像捕获**：高质量照片拍摄
- **WiFi 文件传输**：通过热点在浏览器中直接下载照片
- **软件创意滤镜**：支持 GameBoy、像素、复古胶片等特效（基于 LUT 查表架构）
- **SD卡存储**：将捕获的图像保存到储存卡

## Hardware Requirements
## 硬件要求

- M5Cardputer
- M5 UnitCamS3-5MP camera module
- Micro SD Card
- USB-C cable

- M5Cardputer
- M5 UnitCamS3-5MP 摄像头模块
- Micro SD卡
- USB-C线缆
- 
![unitCamS3-5mp](unitcam.jpg)

## Software Requirements
## 软件要求

- PlatformIO
- ESP32 board support
- Required libraries (listed in platformio.ini)

- PlatformIO开发环境
- ESP32开发板支持
- 所需库文件（列于platformio.ini中）

## Usage
## 使用说明

1. Connect M5Cardputer to your computer
2. Build and upload the project using PlatformIO
3. Insert an SD card into M5Cardputer
4. Power on the device
5. The application will automatically connect to the camera module
6. View the live stream on the screen
7. Capture photos (saved to SD card):
    - **Single Shot**: Short press `BtnA` to capture a high-resolution photo.
    - **Burst Mode**: Long press `BtnA` (> 0.5s) to capture high-resolution photos every 0.2s.
8. Press `0`-`6` number keys to set built-in camera special effect
9. Press `7`, `8`, or `9` to toggle Software Creative Filters (GameBoy, Pixelate, Film). Press the same key again to turn off.
10. Press `w` to enable **WiFi File Transfer Mode**. Connect to `Cardputer-Cam` and visit `http://192.168.4.1` on your phone to download images.
11. Press `` ` `` (backtick) to view camera status and parameter settings
12. Adjust camera parameters using keyboard:

1. 将M5Cardputer连接到电脑
2. 使用PlatformIO构建并上传项目
3. 将Micro SD卡插入M5Cardputer
4. 打开设备电源
5. 应用将自动连接到相机模块
6. 在屏幕上查看实时串流画面
7. 拍摄照片（保存到SD卡）：
    - **单张拍摄**：短按 `BtnA` 按钮。
    - **连拍模式**：长按 `BtnA` 按钮（>0.5秒），每0.2秒自动拍摄一张。
8. 按数字键 `0`-`6` 设置硬件内置特效
9. 按 `7`、`8` 或 `9` 切换软件创意滤镜（GameBoy风、像素风、复古胶片），同键再按即可关闭。
10. 按 `w` 键开启 **WiFi 无线传图模式**。连接 `Cardputer-Cam` 热点后，手机访问 `http://192.168.4.1` 即可预览下载照片。
11. 按 `` ` ``（反引号）查看相机状态和参数设置
12. 使用键盘调整相机参数：

### Camera Parameter Controls
### 相机参数控制

| Parameter | Key (Increase) | Key (Decrease) | Range |
| 参数 | 增加按键 | 减少按键 | 范围 |
|-----------|----------------|----------------|-------|
| Brightness 亮度 | `;` (semicolon) | `.` (period) | -2 to 2 |
| Contrast 对比度 | `/` (slash) | `,` (comma) | -2 to 2 |
| Saturation 饱和度 | `]` (right bracket) | `[` (left bracket) | -2 to 2 |
| Sharpness 锐度 | `=` (equals) | `_` (underscore) | -2 to 2 |
| Special Effect 内置特效 | `0-6` (number keys) | - | 0 to 6 |
| Creative Filter 创意滤镜 | `7`,`8`,`9` | - | Toggle (同键开关) |
| WiFi Transfer 传图模式 | `w` | - | Toggle (AP Mode) |

### Timelapse Mode
### 延时摄影模式

- Press `t` key to start timelapse mode
- Photos are automatically taken every 5 seconds
- Photos are saved to `/images/timelapse` directory
- Display shows photo count, countdown, remaining storage, and battery
- Screen turns off after 1 minute of inactivity (press any key to wake)
- Power off the device to exit timelapse mode and reset camera module

- 按`t`键启动延时摄影模式
- 每5秒自动拍摄一张照片
- 照片保存到`/images/timelapse`目录
- 屏幕显示照片数量、倒计时、剩余存储空间和电量
- 1分钟无操作后屏幕自动熄灭（按任意键唤醒）
- 关闭设备电源退出延时摄影模式并重置摄像头模块

## Configuration
## 配置选项

Resolution settings are defined at the top of `src/main.cpp`:

分辨率设置在`src/main.cpp`文件顶部定义：

```cpp
#define CAMERA_RESOLUTION_HIGH 13     // High resolution for photo capture
#define CAMERA_RESOLUTION_LOW 6       // Low resolution for streaming
```

```cpp
#define CAMERA_RESOLUTION_HIGH 13     // 用于拍摄照片的高分辨率
#define CAMERA_RESOLUTION_LOW 6       // 用于串流的低分辨率
```

## MJPEG Preview Optimization
## MJPEG 预览渲染优化

The application features a heavily optimized MJPEG streaming engine that achieves **7-10 FPS** (standard ESP32-S3 software decoding) with zero screen tearing.

本应用包含一个经过深度优化的 MJPEG 串流引擎，在标准 ESP32-S3 软件解码下可达到 **7-10 FPS** 的帧率，且完全消除了画面撕裂现象。

### Optimization Techniques
### 优化技术点

- **Buffered Batch Reading**: Implemented a 1024-byte local buffer for `WiFiClient` access, significantly reducing system call overhead.
- **Handshake Body Alignment**: Explicitly syncs to the `\r\n\r\n` header terminator before starting the chunked parser, preventing protocol drift (garbled screens) after reboots.
- **SOI/EOI Strong Alignment**: Aggressively scans for `0xFFD8` and `0xFFD9` markers to ensure only valid JPEG payloads reach the decoder.
- **15KB Circuit Breaker**: Discards any illegal frames exceeding 15KB to protect memory from stale stream data.
- **Self-Healing Reconnection**: Automatically restarts the MJPEG stream and resets the parser after 15 consecutive rendering failures.
- **Metric Caching**: Persists the last successful frame size in diagnostics to prevent "0 B" flicker.

- **带缓冲的批量读取**：为 `WiFiClient` 访问实现了 1024 字节的局部缓冲区，将系统调用次数减少了几个数量级，极大提升了吞吐量。
- **响应体强对齐 (\r\n\r\n Sync)**：在连接建立后强制检索 HTTP 响应头结束符，确保解析器从第一个真正的分块头开始解码，彻底消除了重启后的“花屏”现象。
- **SOI/EOI 强对齐**：解析器主动搜索 `0xFFD8` 开头和 `0xFFD9` 结尾，确保即使网络流包含杂质，解码器也只会收到合法的 JPEG 帧。
- **15KB 局帧熔断**：将单帧上限限制为 15KB，一旦由于流偏移检测到异常大包，立即熔断并复位解析器，清除过期数据堆积。
- **自愈式重连逻辑**：若连续 15 帧渲染失败，系统会自动重启 MJPEG 流连接并执行状态机全量重置，确保长时间运行的稳定性。
- **指标静态缓存**：缓存并显示上一帧有效图像的尺寸数据，解决了在统计时 Size 指标频繁跳 0 的误导性问题。


## Creative Filter Implementation (LUT Architecture)
## 创意滤镜实现技术要点（LUT 查表架构）

The creative filter system uses a **Look-Up Table (LUT)** architecture for maximum performance. All color transformations are pre-computed into full RGB565 lookup tables (65,536 entries × 2 bytes = 128 KB each) at build time using a Python generator script (`tools/generate_luts.py`). At runtime, applying a filter reduces to a single memory access per pixel — **zero arithmetic operations**.

创意滤镜系统采用 **LUT（查表）架构** 以实现最高性能。所有颜色变换均在编译期通过 Python 脚本（`tools/generate_luts.py`）预计算为完整的 RGB565 查表（每表 65,536 条目 × 2 字节 = 128 KB）。运行时应用滤镜仅需对每个像素执行一次内存访问——**零算术运算**。

### Architecture / 架构

- **LUT Storage**: All LUT arrays are declared as `static const` and reside entirely in **Flash (.rodata)** via ESP32-S3 MMU mapping. They consume **zero SRAM/PSRAM**.
- **LUT Generation**: A Python script (`tools/generate_luts.py`) iterates all 65,536 possible RGB565 values, applies the filter's color transformation algorithm in floating-point on the PC, and outputs C header files.
- **Unified Render Path**: All pure-color filters share a single `applyLUT()` function — no per-filter branching in the hot loop.

- **LUT 存储**：所有 LUT 数组以 `static const` 声明，完全存放在 **Flash (.rodata)** 中，通过 ESP32-S3 的 MMU 映射直接寻址，**不占用任何 SRAM/PSRAM**。
- **LUT 生成**：Python 脚本（`tools/generate_luts.py`）遍历全部 65,536 种 RGB565 颜色值，在 PC 端以浮点精度执行滤镜色彩变换算法，输出 C 头文件。
- **统一渲染路径**：所有纯色滤镜共享一个 `applyLUT()` 函数，热循环中无分支判断。

```cpp
// Core rendering: one memory access per pixel, zero math
inline uint16_t applyFilter(const uint16_t* lut, uint16_t pixel) {
    return lut[pixel];  // Direct index, O(1)
}
```

---

### 1. GameBoy Filter (GameBoy 经典绿) — Pure LUT
- Pre-computes ITU-R BT.601 grayscale quantization → 4-level green palette mapping for all 65,536 RGB565 inputs.
- Classic palette: `#0f380f`, `#306230`, `#8bac0f`, `#9bbc0f`.
- 预计算 ITU-R BT.601 灰度量化 → 4级绿色调色板映射，覆盖全部 65,536 种 RGB565 输入。
- 经典四色：`#0f380f`、`#306230`、`#8bac0f`、`#9bbc0f`。

### 2. Pixelate Filter (复古像素风) — Hybrid: LUT + Spatial
- **Color mapping (LUT)**: Pre-computes vibrance boost (2.2× saturation pull) + PICO-8 16-color palette matching (weighted Euclidean distance, G×2) for all inputs.
- **Spatial pixelation (algorithmic)**: Divides canvas into 4×4 blocks, samples top-left pixel through LUT, fills entire block with `canvas.fillRect`.
- **颜色映射（LUT）**：预计算鲜艳度增强（2.2× 饱和度拉伸）+ PICO-8 16色调色板匹配（加权欧氏距离，G通道 ×2）。
- **空间像素化（算法）**：将画布分割为 4×4 块，采样左上角像素经 LUT 变换后，使用 `canvas.fillRect` 填充整块。

### 3. Vintage Film Filter (复古胶片暖色调) — Pure LUT
- Pre-computes a multi-step color grade: S-curve contrast → warm color shift (+R, -B) → desaturation fade → shadow lift → highlight yellowing.
- 预计算多步色彩分级：S曲线对比度 → 暖色温偏移（+红 -蓝）→ 褪色降饱和 → 暗部提升 → 高光泛黄。

### Performance / 性能

| Metric | Old Algorithm | New LUT |
|--------|--------------|--------|
| Per-pixel ops | 5-15 arithmetic operations | 1 memory read |
| Flash usage | ~0 KB | +384 KB (3 LUTs) |
| SRAM usage | ~640 bytes (row buffer) | 0 bytes |
| Code complexity | ~267 lines | ~95 lines |


## License

## 许可证

PolyForm Noncommercial License 1.0.0