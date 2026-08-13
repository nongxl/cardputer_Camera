#ifndef INPUT_HANDLER_H
#define INPUT_HANDLER_H

#include "Global.h"

enum InputEvent {
  EVENT_NONE,
  EVENT_RESTART,
  EVENT_TIMELAPSE,
  EVENT_STATUS,
  EVENT_HELP,
  EVENT_BRIGHTNESS_UP,
  EVENT_BRIGHTNESS_DOWN,
  EVENT_CONTRAST_UP,
  EVENT_CONTRAST_DOWN,
  EVENT_SATURATION_UP,
  EVENT_SATURATION_DOWN,
  EVENT_SHARPNESS_UP,
  EVENT_SHARPNESS_DOWN,
  EVENT_EFFECT_START, // 对应特效 0
  EVENT_EFFECT_1,
  EVENT_EFFECT_2,
  EVENT_EFFECT_3,
  EVENT_EFFECT_4,
  EVENT_EFFECT_5,
  EVENT_EFFECT_6,
  EVENT_CAPTURE_START,
  EVENT_CAPTURE_BURST,
  EVENT_CAPTURE_END,
  EVENT_FILTER_GAMEBOY,   // 按键7 → GameBoy风滤镜
  EVENT_FILTER_PIXELATE,  // 按键8 → 像素风滤镜
  EVENT_FILTER_OFF,       // 滤镜再按一次关闭
  EVENT_FILTER_CUSTOM_CYCLE, // 按键9 → 开启/关闭自定义 LUT 滤镜选单
  EVENT_FILTER_LIST_UP,   // 按键分号 (;) → 滤镜菜单向上移动
  EVENT_FILTER_LIST_DOWN, // 按键句号 (.) → 滤镜菜单向下移动
  EVENT_WIFI_SERVER,      // 按键W → 开启 WiFi 文件传输模式
  EVENT_USB_MODE          // 按键U → 开启 USB 磁盘模式
};

class InputHandler {
public:
    static void init();
    static InputEvent handle();
    static bool isKeyPressed(char key);
    static bool wasBtnAPressed();
    static bool isBtnAPressed();
};

#endif // INPUT_HANDLER_H
