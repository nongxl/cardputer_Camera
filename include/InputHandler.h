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
  EVENT_EFFECT_START, // EVENT_EFFECT_0 to EVENT_EFFECT_6
  EVENT_CAPTURE_START,
  EVENT_CAPTURE_BURST,
  EVENT_CAPTURE_END
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
