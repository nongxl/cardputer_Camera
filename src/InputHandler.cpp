#include "InputHandler.h"

void InputHandler::init() {
    // Already done in M5.begin()
}

InputEvent InputHandler::handle() {
    static unsigned long lastKeyPressTime = 0;
    const unsigned long keyDebounceDelay = 200; // 降低防抖延时提高灵敏度
    
    if (M5Cardputer.Keyboard.isChange()) {
        M5Cardputer.Keyboard.updateKeysState();
        
        if (M5Cardputer.Keyboard.isKeyPressed('r')) return EVENT_RESTART;
        if (M5Cardputer.Keyboard.isKeyPressed('t')) return EVENT_TIMELAPSE;
        if (M5Cardputer.Keyboard.isKeyPressed('`')) return EVENT_STATUS;
        if (M5Cardputer.Keyboard.isKeyPressed('h')) return EVENT_HELP;
        
        for (int i = 0; i <= 6; i++) {
            if (M5Cardputer.Keyboard.isKeyPressed('0' + i)) return (InputEvent)(EVENT_EFFECT_START + i);
        }
    }
    
    unsigned long currentTime = millis();
    if (currentTime - lastKeyPressTime >= keyDebounceDelay) {
        if (M5Cardputer.Keyboard.isKeyPressed(';')) { lastKeyPressTime = currentTime; return EVENT_BRIGHTNESS_UP; }
        if (M5Cardputer.Keyboard.isKeyPressed('.')) { lastKeyPressTime = currentTime; return EVENT_BRIGHTNESS_DOWN; }
        if (M5Cardputer.Keyboard.isKeyPressed(',')) { lastKeyPressTime = currentTime; return EVENT_CONTRAST_DOWN; }
        if (M5Cardputer.Keyboard.isKeyPressed('/')) { lastKeyPressTime = currentTime; return EVENT_CONTRAST_UP; }
        if (M5Cardputer.Keyboard.isKeyPressed('[')) { lastKeyPressTime = currentTime; return EVENT_SATURATION_DOWN; }
        if (M5Cardputer.Keyboard.isKeyPressed(']')) { lastKeyPressTime = currentTime; return EVENT_SATURATION_UP; }
        if (M5Cardputer.Keyboard.isKeyPressed('_')) { lastKeyPressTime = currentTime; return EVENT_SHARPNESS_DOWN; }
        if (M5Cardputer.Keyboard.isKeyPressed('=')) { lastKeyPressTime = currentTime; return EVENT_SHARPNESS_UP; }
    }
    
    return EVENT_NONE;
}

bool InputHandler::isKeyPressed(char key) {
    return M5Cardputer.Keyboard.isKeyPressed(key);
}

bool InputHandler::wasBtnAPressed() {
    return M5Cardputer.BtnA.wasPressed();
}

bool InputHandler::isBtnAPressed() {
    return M5Cardputer.BtnA.isPressed();
}
