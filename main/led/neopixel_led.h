#ifndef _NEOPIXEL_LED_H_
#define _NEOPIXEL_LED_H_

#include "led.h"

class NeopixelLed : public Led {
private:
    int pin_;
    int count_;
    int current_r_;
    int current_g_;
    int current_b_;
    bool is_on_;

public:
    NeopixelLed(int pin, int count);
    ~NeopixelLed() override;
    void OnStateChanged() override;
    void SetColor(int r, int g, int b);
    void TurnOff();
    bool IsOn() const { return is_on_; }
};

#endif // _NEOPIXEL_LED_H_
