#include "buzzer_controller.h"
#include "config_manager.h"
#include "../include/config.h"

BuzzerController buzzerController;

BuzzerController::BuzzerController()
    : _pin(BUZZER_PIN), _enabled(false), _beepsLeft(0),
      _beepOn(false), _nextEventMs(0), _lastPatternMs(0),
      _onMs(80), _offMs(80)
{}

void BuzzerController::begin(ConfigManager& config) {
    auto& sys = config.systemConfig();
    _enabled = sys["buzzerEnabled"] | true;
    _pin     = sys["buzzerPin"]     | static_cast<uint8_t>(BUZZER_PIN);
    digitalWrite(_pin, LOW);
    pinMode(_pin, OUTPUT);
}

void BuzzerController::tick() {
    if (!_enabled || _beepsLeft == 0) { return; }
    uint32_t now = millis();
    if ((uint32_t)(now - _nextEventMs) >= 0x80000000UL) { return; } // not yet elapsed
    if (_beepOn) {
        digitalWrite(_pin, LOW);
        _beepOn = false;
        --_beepsLeft;
        if (_beepsLeft > 0) {
            _nextEventMs = now + _offMs;
        }
    } else {
        digitalWrite(_pin, HIGH);
        _beepOn      = true;
        _nextEventMs = now + _onMs;
    }
}

void BuzzerController::_startPattern(uint8_t count, uint16_t onMs, uint16_t offMs) {
    if (!_enabled || count == 0) { return; }
    uint32_t now = millis();
    if (now - _lastPatternMs < 100) { return; } // anti-spam
    _lastPatternMs = now;
    _onMs        = onMs;
    _offMs       = offMs;
    _beepsLeft   = count;
    _beepOn      = false;
    _nextEventMs = now; // fire immediately on next tick
}

void BuzzerController::setEnabled(bool enabled) {
    _enabled = enabled;
    if (!enabled) {
        // silence any in-progress pattern
        _beepsLeft = 0;
        digitalWrite(_pin, LOW);
    }
}

void BuzzerController::beepShort()  { _startPattern(1, 80, 0); }
void BuzzerController::beepDouble() { _startPattern(2, 80, 80); }
void BuzzerController::beepLong()   { _startPattern(1, 600, 0); }
