/*
 * relay_controller.h — GPIO relay management: state, interlock, pulse mode,
 *                      power-on restore, and state-change notifications.
 *
 * Target : ESP8266 (ESP-12F / NodeMCU), Arduino Core 3.x, C++17
 * Build  : PlatformIO (espressif8266)
 *
 * Design constraints
 * ------------------
 *  - No delay() calls anywhere — all timing via millis() in tick().
 *  - No dynamic allocation after begin() — static arrays sized to MAX_CHANNELS.
 *  - GPIO is written BEFORE pinMode to avoid output glitch on init
 *    (write the off-state value first, then set pin as OUTPUT).
 *  - RELAY_ACTIVE_LOW is a compile-time constant from config.h.
 *  - State persistence uses /relays_state.json via ConfigManager.
 */

#ifndef RELAY_CONTROLLER_H_
#define RELAY_CONTROLLER_H_

#include <Arduino.h>
#include <functional>
#include "../include/config.h"

// Forward declaration — avoids circular include; the .cpp includes the full
// config_manager.h where the concrete type is needed.
class ConfigManager;

/* -------------------------------------------------------------------------
 * RelayChannel — per-channel configuration (mirrors data-model.md)
 * ------------------------------------------------------------------------- */
struct RelayChannel {
    uint8_t  id;              // 1-based channel identifier (1–4)
    uint8_t  pin;             // GPIO output pin
    char     name[64];        // Channel label (UTF-8, max 20 code-points + NUL)
    char     powerOnState[8]; // "last" | "on" | "off"
    uint32_t pulseDuration;   // Auto-off delay in ms; 0 = disabled
    uint8_t  interlockGroup;  // 0 = no interlock; 1-255 = group id

    // Runtime state (not persisted in relays.json)
    bool     state;           // Logical target ON/OFF
    bool     gpioState;       // Actual physical GPIO level (logical)
    uint32_t pulseEndTime;    // millis() deadline; 0 = no active pulse
};

/* -------------------------------------------------------------------------
 * RelayController
 * ------------------------------------------------------------------------- */
class RelayController {
public:
    RelayController();

    /*
     * begin() — must be called once from setup() after LittleFS is mounted.
     *
     * Reads /relays.json channel definitions via config, initialises each GPIO
     * in the safe off-state (no output glitch), then restores the power-on
     * state per channel policy.
     */
    void begin(ConfigManager& config);

    /*
     * setState() — the single authoritative path for every relay change.
     *
     * Execution order:
     *   1. Interlock:  if newState=true and interlockGroup>0, turn off all
     *                  other channels in the same group first.
     *   2. GPIO write: apply RELAY_ACTIVE_LOW polarity.
     *   3. In-memory:  update channel.state.
     *   4. Pulse arm:  if newState=true and pulseDuration>0, set pulseEndTime.
     *   5. Persist:    write /relays_state.json via ConfigManager.
     *   6. Callback:   invoke onStateChange if registered.
     *
     * channel is 1-based.  Returns false for invalid channel index.
     */
    bool setState(uint8_t channel, bool newState);

    /*
     * toggle() — convenience; calls setState(!currentState).
     */
    bool toggle(uint8_t channel);

    /*
     * setAll() — applies newState to every configured channel.
     * Hardware actions are batched to ensure simultaneous physical clicking.
     */
    void setAll(bool newState);

    /*
     * Batch operations — use these to change multiple relays at once
     * with only ONE Flash write and one physical action group.
     */
    void startBatch();
    void endBatch();

    /*
     * getState() — returns current in-memory ON/OFF for a 1-based channel.
     * Returns false for an invalid channel.
     */
    bool getState(uint8_t channel) const;

    /*
     * tick() — call from loop() on every iteration.
     * Checks active pulse timers; calls setState(ch, false) when expired.
     * Never blocks.
     */
    void tick();

    /*
     * getChannelCount() — number of successfully initialised channels.
     */
    uint8_t getChannelCount() const;

    /*
     * getChannel() — returns a const pointer to the channel struct for the
     * given 1-based id, or nullptr if not found.
     */
    const RelayChannel* getChannel(uint8_t id) const;

    /*
     * setOnStateChange() — register a callback invoked after every state
     * change.  Intended for WebSocket broadcast.
     * Signature: void callback(uint8_t channelId, bool newState)
     */
    void setOnStateChange(std::function<void(uint8_t, bool)> cb);

private:
    // Channels are stored in a fixed-size array — no heap allocation.
    RelayChannel  _channels[MAX_CHANNELS];
    uint8_t       _channelCount;

    // Pointer to ConfigManager stored during begin(); never owned.
    ConfigManager* _config;

    // Optional state-change notification callback.
    std::function<void(uint8_t, bool)> _onStateChange;

    bool     _isBatchMode     = false;
    bool     _staggerActive   = false;
    uint32_t _lastStaggerMs   = 0;
    bool     _targetStates[MAX_CHANNELS];
    bool     _statePersistPending = false;
    uint32_t _lastStateChangeMs   = 0;

    /*
     * _findByChannel() — internal lookup by 1-based channel id.
     * Returns nullptr if not found.
     */
    RelayChannel* _findByChannel(uint8_t channel);
    const RelayChannel* _findByChannel(uint8_t channel) const;

    /*
     * _applyGpio() — writes the correct physical level for the given logical
     * state, honouring RELAY_ACTIVE_LOW polarity.
     */
    void _applyGpio(const RelayChannel& ch, bool logicalState) const;

    /*
     * _persistState() — serialises the current state of all channels to
     * /relays_state.json via ConfigManager.
     */
    void _persistState();
    void _schedulePersistState();

    /*
     * _loadLastState() — reads /relays_state.json and returns the stored
     * state for the given 1-based channel id.  Returns false on any error
     * (safe default: off).
     */
    bool _loadLastState(uint8_t channelId);

    /*
     * _isBootSensitiveGpio() — returns true if the given pin appears in
     * BOOT_SENSITIVE_GPIOS.  Used for warning logs only; init still proceeds.
     */
    static bool _isBootSensitiveGpio(uint8_t pin);
};

#endif /* RELAY_CONTROLLER_H_ */
