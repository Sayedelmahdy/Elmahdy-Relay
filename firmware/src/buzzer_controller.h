#ifndef BUZZER_CONTROLLER_H_
#define BUZZER_CONTROLLER_H_

#include <Arduino.h>

class ConfigManager;

class BuzzerController {
public:
    BuzzerController();
    ~BuzzerController() = default;

    void begin(ConfigManager& config);
    void tick();
    void setEnabled(bool enabled);

    void beepShort();   // one short beep — relay toggle feedback
    void beepDouble();  // two beeps — WiFi connected
    void beepLong();    // one long beep — reset feedback

private:
    uint8_t  _pin;
    bool     _enabled;
    uint8_t  _beepsLeft;    // number of on+off cycles remaining
    bool     _beepOn;       // current output state
    uint32_t _nextEventMs;  // millis() target for next state change
    uint32_t _lastPatternMs; // anti-spam: 100 ms minimum between patterns
    uint16_t _onMs;
    uint16_t _offMs;

    void _startPattern(uint8_t count, uint16_t onMs, uint16_t offMs);
};

extern BuzzerController buzzerController;

#endif /* BUZZER_CONTROLLER_H_ */
