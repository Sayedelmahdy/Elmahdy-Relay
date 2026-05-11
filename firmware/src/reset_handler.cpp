#include "reset_handler.h"
#include "config_manager.h"
#include "../include/config.h"

ResetHandler resetHandler;

static const uint32_t RESET_DEBOUNCE_MS = 50;
static const uint32_t RESET_REBOOT_MS = 3000;
static const uint32_t RESET_FACTORY_MS = 10000;

ResetHandler::ResetHandler()
    : _pin(RESET_PIN), _enabled(false), _lastState(HIGH),
      _lastReading(HIGH), _pressed(false), _actionTaken(false),
      _pressStartMs(0), _lastChangeMs(0), _config(nullptr)
{}

void ResetHandler::begin(ConfigManager& config) {
    _config  = &config;
    auto& sys = config.systemConfig();
    _enabled = sys["resetEnabled"] | false;
    _pin     = sys["resetPin"]     | static_cast<uint8_t>(RESET_PIN);

    // GPIO16 has no internal pull-up; use an external resistor on D0/GPIO16.
    pinMode(_pin, _pin == 16 ? INPUT : INPUT_PULLUP);
    _lastReading = digitalRead(_pin);
    _lastState = _lastReading;
    _lastChangeMs = millis();

    // A button already held at boot is a valid press for factory reset.
    if (_lastState == LOW) {
        _pressed = true;
        _pressStartMs = _lastChangeMs;
    }
}

void ResetHandler::tick() {
    if (!_enabled || !_config) { return; }

    const uint32_t now = millis();
    const bool reading = digitalRead(_pin);

    if (reading != _lastReading) {
        _lastReading = reading;
        _lastChangeMs = now;
        return;
    }

    if ((now - _lastChangeMs) < RESET_DEBOUNCE_MS) {
        return;
    }

    if (reading == _lastState) {
        if (_pressed && !_actionTaken && (now - _pressStartMs) >= RESET_FACTORY_MS) {
            _actionTaken = true;
            Serial.println(F("[reset] Factory reset triggered (>10s)"));
            _config->resetAll();
            delay(200);
            ESP.restart();
        }
        return;
    }

    const bool state = reading;
    if (state == LOW && _lastState == HIGH) {
        _pressed = true;
        _actionTaken = false;
        _pressStartMs = now;
    } else if (state == HIGH && _lastState == LOW && _pressed) {
        _pressed = false;
        const uint32_t held = now - _pressStartMs;

        if (_actionTaken) {
            // Already handled while the button was held.
        } else if (held >= RESET_FACTORY_MS) {
            Serial.println(F("[reset] Factory reset triggered (>10s)"));
            _config->resetAll();
            delay(200);
            ESP.restart();
        } else if (held >= RESET_REBOOT_MS) {
            Serial.println(F("[reset] Reboot triggered (3-10s)"));
            delay(100);
            ESP.restart();
        } else {
            Serial.printf_P(PSTR("[reset] Short press (%ums) ignored\n"),
                            static_cast<unsigned>(held));
        }
    }

    _lastState = state;
}
