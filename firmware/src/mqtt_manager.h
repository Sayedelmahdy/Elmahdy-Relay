/*
 * mqtt_manager.h — AsyncMqttClient wrapper: HA auto-discovery, LWT, topic
 *                  routing, and exponential-backoff reconnect.
 *
 * Target : ESP8266 (ESP-12F / NodeMCU), Arduino Core 3.x, C++17
 * Build  : PlatformIO (espressif8266 + AsyncMqttClient)
 *
 * Design notes
 * ─────────────
 * • AsyncMqttClient is a direct member (not pointer) — avoids one heap
 *   allocation and keeps the object in BSS.
 * • All timing uses millis() in tick(); no delay() anywhere.
 * • Reconnect backoff: 1 s → 2 s → 4 s → 8 s → 16 s → 30 s cap.
 *   Resets to 1 s on the first successful connect acknowledgement.
 * • ISR / callback safety: AsyncMqttClient callbacks run from the TCP/IP
 *   stack interrupt context on ESP8266.  We only set flags and call
 *   non-blocking publish/subscribe here — no Serial.print in hot paths.
 * • Forward declarations for SceneManager and TimerEngine avoid pulling
 *   their headers (which do not exist yet) into every translation unit.
 * • The `_relay` onStateChange callback is registered in begin() so that
 *   every relay GPIO change automatically publishes a retained status topic.
 */

#ifndef MQTT_MANAGER_H_
#define MQTT_MANAGER_H_

#include <Arduino.h>
#include <functional>
#include <AsyncMqttClient.h>

// Forward declarations — full headers included only in mqtt_manager.cpp
class ConfigManager;
class RelayController;
// SceneManager and TimerEngine are not implemented yet (T045, T032).
// Their stubs are referenced only in onMessage topic dispatch.
class SceneManager;
class TimerEngine;

/* ─────────────────────────────────────────────────────────────────────────────
 * Backoff constants
 * ───────────────────────────────────────────────────────────────────────────*/
static constexpr uint32_t MQTT_BACKOFF_MIN_MS = 1000UL;   // 1 s initial delay
static constexpr uint32_t MQTT_BACKOFF_MAX_MS = 30000UL;  // 30 s ceiling

/* ─────────────────────────────────────────────────────────────────────────────
 * MqttManager
 * ───────────────────────────────────────────────────────────────────────────*/
class MqttManager {
public:
    MqttManager();
    ~MqttManager() = default;

    /*
     * begin() — configure AsyncMqttClient and initiate the first connection.
     *
     * If MQTT is not enabled in config ("enabled" == false) this returns
     * immediately without touching the network stack.
     *
     * Registers a RelayController onStateChange callback so every GPIO
     * transition triggers publishRelayState() automatically.
     *
     * Parameters
     *   config — source of /mqtt.json broker/credentials/prefix settings.
     *   relay  — used to read channel count, states, and register callbacks.
     */
    void begin(ConfigManager& config, RelayController& relay);

    /*
     * reloadFromConfig() — re-read broker/port/prefix from ConfigManager and
     * reconnect immediately. Used after /api/config/mqtt saves settings.
     */
    void reloadFromConfig();

    /*
     * tick() — must be called from loop() on every iteration.
     *
     * Drives the reconnect backoff state machine: if MQTT is enabled but
     * disconnected, checks whether _backoffDeadlineMs has elapsed and calls
     * _mqtt.connect() when it has.  Never blocks.
     */
    void tick();

    /*
     * isConnected() — returns true when the broker has acknowledged the
     * CONNECT packet (i.e., the onConnect callback has fired and no
     * subsequent onDisconnect has been received).
     */
    bool isConnected() const;
    bool isEnabled() const { return _enabled; }
    const String& broker() const { return _broker; }
    uint16_t port() const { return _port; }
    const String& prefix() const { return _prefix; }

    /*
     * publishRelayState() — publish `{prefix}/relay/{ch}/status` with
     * payload "ON" or "OFF", QoS 1, retained.
     *
     * Called automatically by the RelayController onStateChange callback
     * registered in begin().  May also be called directly (e.g., on connect
     * to sync all states).
     *
     * ch is 1-based.  Silently ignored when not connected or MQTT disabled.
     */
    void publishRelayState(uint8_t ch, bool state);

    /*
     * publishAllStates() — iterate every channel and call publishRelayState().
     * Typically called on reconnect to bring the broker up to date.
     */
    void publishAllStates();

    /*
     * setOnMessage() — register an optional application-level callback that
     * receives every inbound MQTT message after internal dispatch.
     * Useful for bridging to the WebSocket handler in a later task.
     */
    void setOnMessage(std::function<void(const char* topic,
                                        const char* payload)> cb);

    /*
     * setSceneManager() / setTimerEngine() — wire in module references so
     * MQTT topic handlers can call activate() and cancel() respectively (T047).
     * Must be called from setup() after both modules are initialised.
     */
    void setSceneManager(SceneManager& scene) { _sceneManager = &scene; }
    void setTimerEngine(TimerEngine& engine)  { _timerEngine  = &engine; }

private:
    /* ── AsyncMqttClient instance (direct member, no heap) ───────────────── */
    AsyncMqttClient _mqtt;

    /* ── Weak references set in begin() ──────────────────────────────────── */
    ConfigManager*  _config;
    RelayController* _relay;

    /* ── Cached config values (copied from JsonDocument in begin()) ────────
     * Stored as Arduino String so the char* passed to AsyncMqttClient
     * remains valid for the lifetime of this object.               */
    bool     _enabled;
    String   _broker;
    uint16_t _port;
    String   _username;
    String   _password;
    String   _prefix;        // e.g. "elmahdy"
    String   _deviceId;      // "elmahdy_relay_" + MAC (no colons, lowercase)
    String   _lwtTopic;      // Persist the Will Topic so setWill pointer remains valid

    /* ── Reconnect backoff ────────────────────────────────────────────────── */
    bool     _connected;          // mirrored from onConnect / onDisconnect
    bool     _callbacksRegistered = false;
    uint32_t _backoffMs;          // current backoff interval
    uint32_t _backoffDeadlineMs;  // millis() target for next connect attempt
    bool     _reconnectPending;   // true when a timed reconnect is scheduled

    /* ── Optional passthrough callback ───────────────────────────────────── */
    std::function<void(const char*, const char*)> _onMessageCb;

    /* ── Module references for topic dispatch (T047) ─────────────────────── */
    SceneManager* _sceneManager = nullptr;
    TimerEngine*  _timerEngine  = nullptr;

    /* ── Internal helpers ────────────────────────────────────────────────── */

    /*
     * _buildTopic() — assemble a complete topic string from the configured
     * prefix and the provided suffix, returning it as an Arduino String.
     * Example: _buildTopic("relay/1/status") → "elmahdy/relay/1/status"
     */
    String _buildTopic(const char* suffix) const;
    void _loadConfig();
    void _configureClient();

    /*
     * _publishHaDiscovery() — publish Home Assistant MQTT discovery payloads
     * for every relay channel (T028).  Called on every successful connect.
     * Each payload is published to:
     *   homeassistant/switch/{_deviceId}/relay_{ch}/config
     * with retain=true, QoS=1.
     */
    void _publishHaDiscovery();

    /*
     * _scheduleReconnect() — double _backoffMs (capped at MQTT_BACKOFF_MAX_MS)
     * and set _backoffDeadlineMs = millis() + _backoffMs.
     */
    void _scheduleReconnect();

    /* ── AsyncMqttClient event handlers (registered as lambdas in begin()) ─ */
    void _onConnect(bool sessionPresent);
    void _onDisconnect(AsyncMqttClientDisconnectReason reason);
    void _onMessage(char* topic, char* payload,
                    AsyncMqttClientMessageProperties properties,
                    size_t len, size_t index, size_t total);
};

#endif /* MQTT_MANAGER_H_ */
