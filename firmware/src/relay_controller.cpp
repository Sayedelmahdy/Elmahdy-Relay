/*
 * relay_controller.cpp — Implementation of RelayController.
 *
 * Target : ESP8266 (ESP-12F / NodeMCU), Arduino Core 3.x, C++17
 * Build  : PlatformIO (espressif8266)
 *
 * Key design decisions
 * --------------------
 *  Boot-glitch safety
 *      We write the GPIO output register BEFORE calling pinMode().
 *      On the ESP8266, setting the output latch before enabling the output
 *      driver prevents a brief LOW/HIGH glitch during init that could
 *      momentarily energise a relay coil.
 *
 *  RELAY_ACTIVE_LOW polarity
 *      When RELAY_ACTIVE_LOW == true (the typical optocoupler module):
 *          logical ON  → GPIO LOW
 *          logical OFF → GPIO HIGH
 *      _applyGpio() centralises this conversion so no other code needs
 *      to reason about polarity.
 *
 *  Interlock ordering
 *      When turning ON a channel that belongs to an interlock group, every
 *      other channel in the same group is de-energised via _applyGpio() +
 *      direct field writes — NOT by recursing into setState().  This keeps
 *      the call tree flat (zero recursion), avoids spurious intermediate
 *      _persistState() and _onStateChange() calls for the silenced peers,
 *      and makes the no-recursion guarantee unconditional.
 *
 *  Pulse mode
 *      pulseEndTime is armed to millis() + pulseDuration only when
 *      turning ON.  tick() compares millis() against pulseEndTime every
 *      loop iteration — the resolution is the loop cadence (~1 ms typical
 *      on ESP8266 at 80 MHz with no blocking code).
 *
 *  Persistence (/relays_state.json)
 *      JSON structure (written by _persistState, read by _loadLastState):
 *      {
 *        "states": [false, true, false, false],
 *        "crc": <uint32>
 *      }
 *      Index 0 = channel id 1, index 1 = channel id 2, etc.
 *      The array is always MAX_CHANNELS (4) entries long for fixed layout.
 */

#include "relay_controller.h"
#include "config_manager.h"   // Full include needed here for method calls

#include <ArduinoJson.h>

static const uint32_t RELAY_STATE_SAVE_DEBOUNCE_MS = 1000;

/* =========================================================================
 * Constructor
 * ========================================================================= */

RelayController::RelayController()
    : _channelCount(0)
    , _config(nullptr)
    , _onStateChange(nullptr)
    , _isBatchMode(false)
{
    // Zero-initialise the channel array so every field starts in a known state
    // even if begin() is called late or not at all.
    memset(_channels, 0, sizeof(_channels));
    for (uint8_t i = 0; i < MAX_CHANNELS; i++) _targetStates[i] = false;
}

/* =========================================================================
 * begin()
 * ========================================================================= */

void RelayController::begin(ConfigManager& config) {
    _config = &config;
    _channelCount = 0;

    // ---- 1. Load channel definitions from /relays.json ------------------
    //
    // ConfigManager::getRelaysJson() returns a const reference to a
    // JsonDocument that holds the parsed /relays.json content.  We read
    // channelCount and the channels[] array from it.

    const JsonDocument& doc = _config->relayConfig();

    uint8_t cfgCount = doc["channelCount"] | static_cast<uint8_t>(DEFAULT_CHANNEL_COUNT);
    if (cfgCount == 0 || cfgCount > MAX_CHANNELS) {
        cfgCount = DEFAULT_CHANNEL_COUNT;
    }

    // Default GPIO assignments matching config.h (pins 5,4,14,12 for ch 1-4)
    static const uint8_t DEFAULT_PINS[MAX_CHANNELS] = {
        RELAY_1_PIN, RELAY_2_PIN, RELAY_3_PIN, RELAY_4_PIN
    };

    for (uint8_t i = 0; i < cfgCount; i++) {
        RelayChannel& ch = _channels[i];

        if (doc["channels"][i].is<JsonObjectConst>()) {
            JsonObjectConst obj = doc["channels"][i].as<JsonObjectConst>();

            ch.id             = obj["id"]             | static_cast<uint8_t>(i + 1);
            ch.pin            = obj["pin"]             | DEFAULT_PINS[i];
            ch.pulseDuration  = obj["pulseDuration"]  | static_cast<uint32_t>(0);
            ch.interlockGroup = obj["interlockGroup"]  | static_cast<uint8_t>(0);

            const char* name = obj["name"] | "";
            strncpy(ch.name, name, sizeof(ch.name) - 1);
            ch.name[sizeof(ch.name) - 1] = '\0';

            const char* pos = obj["powerOnState"] | "last";
            strncpy(ch.powerOnState, pos, sizeof(ch.powerOnState) - 1);
            ch.powerOnState[sizeof(ch.powerOnState) - 1] = '\0';
        } else {
            // Fall back to defaults if JSON is absent or malformed.
            ch.id             = static_cast<uint8_t>(i + 1);
            ch.pin            = DEFAULT_PINS[i];
            ch.pulseDuration  = 0;
            ch.interlockGroup = 0;
            snprintf(ch.name, sizeof(ch.name), "Channel %u", i + 1);
            strncpy(ch.powerOnState, "last", sizeof(ch.powerOnState));
        }

        ch.state       = false;
        ch.pulseEndTime = 0;

        // ---- 2. Boot-glitch-safe GPIO init ------------------------------
        //
        // Write the OFF-state level FIRST (before pinMode) so the output
        // latch is preloaded.  Then enable the output driver.

        if (_isBootSensitiveGpio(ch.pin)) {
            Serial.printf(
                "[RelayController] WARNING: channel %u uses boot-sensitive GPIO %u\n",
                ch.id, ch.pin);
        }

        // Pre-set the output register to the logical-OFF level.
        bool offLevel = RELAY_ACTIVE_LOW ? HIGH : LOW;
        digitalWrite(ch.pin, offLevel);
        pinMode(ch.pin, OUTPUT);

        _channelCount++;
    }

    // ---- 3. Restore power-on state per channel --------------------------
    for (uint8_t i = 0; i < _channelCount; i++) {
        RelayChannel& ch = _channels[i];
        bool target = false;

        if (strncmp(ch.powerOnState, "on", 2) == 0) {
            target = true;
        } else if (strncmp(ch.powerOnState, "off", 3) == 0) {
            target = false;
        } else {
            // "last" — read the persisted state; default off on any error
            target = _loadLastState(ch.id);
        }

        // Use the internal GPIO-write path directly rather than the full
        // setState() so we avoid callback noise and a redundant persist
        // write during startup — the stored state is already correct.
        _applyGpio(ch, target);
        ch.state = target;
        ch.gpioState = target;
        _targetStates[i] = target;
        ch.pulseEndTime = 0;
    }

    Serial.printf("[RelayController] Initialised %u channel(s)\n", _channelCount);
}

/* =========================================================================
 * setState()
 * ========================================================================= */

bool RelayController::setState(uint8_t channel, bool newState) {
    RelayChannel* ch = _findByChannel(channel);
    if (ch == nullptr) {
        Serial.printf("[RelayController] setState: channel %u not found\n", channel);
        return false;
    }

    if (!_isBatchMode && ch->state == newState && ch->gpioState == newState) {
        return true;
    }

    // ---- 1. Interlock enforcement ----------------------------------------
    //
    // If we are turning ON a channel that belongs to an interlock group,
    // turn OFF every other channel in that group first — directly via
    // _applyGpio() + field write, without recursing into setState().
    //
    // Rationale: calling setState() recursively for each peer would fire
    // _persistState() and _onStateChange for every peer turn-off before
    // the initiating channel is even applied.  The spec requires the
    // initiating channel's callback to be the only one emitted; peers are
    // silently de-energised as a side-effect of the interlock.  Direct
    // manipulation also makes the no-recursion guarantee unconditional —
    // it does not rely on the newState guard remaining correct if this
    // function is ever refactored.

    if (newState && ch->interlockGroup != 0) {
        for (uint8_t i = 0; i < _channelCount; i++) {
            RelayChannel& peer = _channels[i];
            if (peer.id != ch->id &&
                peer.interlockGroup == ch->interlockGroup &&
                peer.state == true)
            {
                // Drive GPIO off and update in-memory state directly.
                // pulseEndTime is also cleared so a pending auto-off
                // tick does not resurrect a relay the interlock just
                // silenced.
                _applyGpio(peer, false);
                peer.state       = false;
                peer.pulseEndTime = 0;
            }
        }
    }

    // ---- 2. Write GPIO --------------------------------------------------
    if (!_isBatchMode && !_staggerActive) {
        _applyGpio(*ch, newState);
        ch->gpioState = newState;
    }

    // ---- 3. Update in-memory state --------------------------------------
    ch->state = newState;
    _targetStates[static_cast<uint8_t>(channel - 1)] = newState;

    // ---- 4. Arm / disarm pulse timer ------------------------------------
    if (newState && ch->pulseDuration > 0) {
        ch->pulseEndTime = millis() + ch->pulseDuration;
    } else {
        // Clear any prior pulse when explicitly turned off or pulse disabled
        ch->pulseEndTime = 0;
    }

    // ---- 5. Persist state -----------------------------------------------
    if (!_isBatchMode) {
        _schedulePersistState();
    }

    // ---- 6. Invoke callback ---------------------------------------------
    if (_onStateChange) {
        _onStateChange(ch->id, newState);
    }

    return true;
}

/* =========================================================================
 * toggle()
 * ========================================================================= */

bool RelayController::toggle(uint8_t channel) {
    const RelayChannel* ch = _findByChannel(channel);
    if (ch == nullptr) {
        return false;
    }
    return setState(channel, !ch->state);
}

/* =========================================================================
 * setAll()
 * ========================================================================= */

void RelayController::setAll(bool newState) {
    startBatch();
    for (uint8_t i = 0; i < _channelCount; i++) {
        setState(_channels[i].id, newState);
    }
    endBatch();
}

void RelayController::startBatch() {
    _isBatchMode = true;
}

void RelayController::endBatch() {
    if (!_isBatchMode) return;
    _isBatchMode = false;
    
    // Start the non-blocking staggered sequence
    _staggerActive = true;
    _lastStaggerMs = 0; // Trigger first one immediately
}

/* =========================================================================
 * getState()
 * ========================================================================= */

bool RelayController::getState(uint8_t channel) const {
    const RelayChannel* ch = _findByChannel(channel);
    return (ch != nullptr) ? ch->state : false;
}

/* =========================================================================
 * tick()
 * ========================================================================= */

void RelayController::tick() {
    const uint32_t now = millis();

    // ---- 1. Staggered Batch Execution (Non-blocking) --------------------
    if (_staggerActive) {
        if (_lastStaggerMs == 0 || (now - _lastStaggerMs >= RELAY_STAGGER_MS)) {
            bool anyChangedThisTick = false;
            bool anyPendingAtAll = false;

            for (uint8_t i = 0; i < _channelCount; i++) {
                RelayChannel& ch = _channels[i];
                if (ch.gpioState != ch.state) {
                    if (!anyChangedThisTick) {
                        // Apply this relay now
                        _applyGpio(ch, ch.state);
                        ch.gpioState = ch.state;
                        _lastStaggerMs = now;
                        anyChangedThisTick = true;
                    } else {
                        // More relays are still waiting for their turn
                        anyPendingAtAll = true;
                    }
                }
            }

            if (!anyChangedThisTick && !anyPendingAtAll) {
                // Everything is now in sync
                _staggerActive = false;
                _persistState();
            }
        }
    }

    if (!_staggerActive && _statePersistPending &&
        (now - _lastStateChangeMs) >= RELAY_STATE_SAVE_DEBOUNCE_MS) {
        _persistState();
    }

    // ---- 2. Pulse Mode Expiry -------------------------------------------
    for (uint8_t i = 0; i < _channelCount; i++) {
        RelayChannel& ch = _channels[i];
        if (ch.pulseEndTime != 0 && now >= ch.pulseEndTime) {
            // Clear before setState() to prevent spurious re-entry.
            ch.pulseEndTime = 0;
            setState(ch.id, false);
        }
    }
}

/* =========================================================================
 * getChannelCount()
 * ========================================================================= */

uint8_t RelayController::getChannelCount() const {
    return _channelCount;
}

/* =========================================================================
 * getChannel()
 * ========================================================================= */

const RelayChannel* RelayController::getChannel(uint8_t id) const {
    return _findByChannel(id);
}

/* =========================================================================
 * setOnStateChange()
 * ========================================================================= */

void RelayController::setOnStateChange(std::function<void(uint8_t, bool)> cb) {
    _onStateChange = cb;
}

/* =========================================================================
 * Private helpers
 * ========================================================================= */

RelayChannel* RelayController::_findByChannel(uint8_t channel) {
    for (uint8_t i = 0; i < _channelCount; i++) {
        if (_channels[i].id == channel) {
            return &_channels[i];
        }
    }
    return nullptr;
}

const RelayChannel* RelayController::_findByChannel(uint8_t channel) const {
    for (uint8_t i = 0; i < _channelCount; i++) {
        if (_channels[i].id == channel) {
            return &_channels[i];
        }
    }
    return nullptr;
}

void RelayController::_applyGpio(const RelayChannel& ch, bool logicalState) const {
    // RELAY_ACTIVE_LOW is a compile-time bool constant from config.h.
    // When true: ON  = drive LOW, OFF = drive HIGH.
    // When false:ON  = drive HIGH, OFF = drive LOW.
    int physicalLevel = RELAY_ACTIVE_LOW ? (logicalState ? LOW : HIGH)
                                         : (logicalState ? HIGH : LOW);
    digitalWrite(ch.pin, physicalLevel);
}

void RelayController::_persistState() {
    if (_config == nullptr) {
        return;
    }

    // Build a compact JSON document.
    // Format:
    //   { "states": [<bool>, <bool>, <bool>, <bool>] }
    // The array is always MAX_CHANNELS long (padded with false) so that
    // channel indices are stable regardless of current channelCount.
    // CRC is appended by ConfigManager::saveJson() internally.

    JsonDocument& doc = _config->relayStateConfigMut();
    doc.clear();
    JsonArray arr = doc["states"].to<JsonArray>();
    for (uint8_t i = 0; i < MAX_CHANNELS; i++) {
        if (i < _channelCount) {
            arr.add(_channels[i].state);
        } else {
            arr.add(false);
        }
    }

    if (_config->saveRelayState()) {
        _statePersistPending = false;
    }
}

void RelayController::_schedulePersistState() {
    _statePersistPending = true;
    _lastStateChangeMs = millis();
}

bool RelayController::_loadLastState(uint8_t channelId) {
    if (_config == nullptr) {
        return false;
    }

    const JsonDocument& doc = _config->relayStateConfig();
    JsonArrayConst arr = doc["states"].as<JsonArrayConst>();
    if (arr.isNull()) {
        return false;
    }

    // channelId is 1-based; array index is 0-based.
    uint8_t idx = static_cast<uint8_t>(channelId - 1);
    if (idx >= MAX_CHANNELS) {
        return false;
    }

    // Use the || false default so missing or null entries map to off.
    return arr[idx] | false;
}

bool RelayController::_isBootSensitiveGpio(uint8_t pin) {
    for (uint8_t i = 0; i < BOOT_SENSITIVE_GPIO_COUNT; i++) {
        if (BOOT_SENSITIVE_GPIOS[i] == pin) {
            return true;
        }
    }
    return false;
}
