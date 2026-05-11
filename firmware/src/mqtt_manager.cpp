/*
 * mqtt_manager.cpp — AsyncMqttClient implementation for Elmahdy Relay.
 *
 * Implements T027 (MqttManager core) and T028 (HA auto-discovery).
 *
 * Topic contract (prefix configurable, default "elmahdy"):
 *
 *   Subscribe  {prefix}/relay/{ch}/control       payload ON / OFF / TOGGLE
 *   Subscribe  {prefix}/relay/all/control        payload ON / OFF
 *   Subscribe  {prefix}/scene/{name}/control     payload ON  (stub → T045)
 *   Subscribe  {prefix}/timer/{id}/control       payload CANCEL  (stub → T032)
 *
 *   Publish    {prefix}/relay/{ch}/status        ON / OFF  (retained, QoS 1)
 *   Publish    {prefix}/system/status            online / offline  (LWT, retained)
 *   Publish    {prefix}/system/info              JSON  (version / uptime / rssi / ip)
 *
 *   Publish    homeassistant/switch/{device_id}/relay_{ch}/config
 *              HA discovery JSON  (retained, QoS 1)  — T028
 */

#include "mqtt_manager.h"
#include "config_manager.h"
#include "relay_controller.h"
#include "scene_manager.h"
#include "timer_engine.h"

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ArduinoJson.h>

#include "../include/config.h"   // VERSION, MAX_CHANNELS

/* ─────────────────────────────────────────────────────────────────────────────
 * Constructor
 * ───────────────────────────────────────────────────────────────────────────*/

MqttManager::MqttManager()
    : _config(nullptr),
      _relay(nullptr),
      _enabled(false),
      _port(1883),
      _connected(false),
      _backoffMs(MQTT_BACKOFF_MIN_MS),
      _backoffDeadlineMs(0),
      _reconnectPending(false)
{}

/* ─────────────────────────────────────────────────────────────────────────────
 * begin()
 * ───────────────────────────────────────────────────────────────────────────*/

void MqttManager::begin(ConfigManager& config, RelayController& relay) {
    _config = &config;
    _relay  = &relay;

    // ── Read MQTT settings from in-memory config doc ─────────────────────────
    const JsonDocument& mDoc = _config->mqttConfig();
    _enabled  = mDoc["enabled"]  | false;
    _broker   = mDoc["broker"]   | "broker.emqx.io";
    _port     = mDoc["port"]     | static_cast<uint16_t>(1883);
    _username = mDoc["username"] | "";
    _password = mDoc["password"] | "";
    _prefix   = mDoc["prefix"]  | "elmahdy";

    if (!_enabled) {
        Serial.println(F("[MQTT] Disabled in config — skipping init"));
        // Keep callbacks configured so MQTT can be enabled from the dashboard.
    }

    // ── Build device_id from MAC: "elmahdy_relay_" + MAC without colons ──────
    // WiFi.macAddress() returns e.g. "AA:BB:CC:DD:EE:FF"
    {
        String mac = WiFi.macAddress();
        mac.replace(":", "");
        mac.toLowerCase();
        // MQTT 3.1.1 max Client ID length is 23 characters.
        // "ER_" (3) + MAC (12) = 15 characters, which is safe.
        _deviceId = "ER_" + mac;
    }

    // ── Configure AsyncMqttClient ─────────────────────────────────────────────
    _mqtt.setServer(_broker.c_str(), _port);
    _mqtt.setClientId(_deviceId.c_str());   // unique per device (MAC-based)
    _mqtt.setCredentials(
        _username.length() ? _username.c_str() : nullptr,
        _password.length() ? _password.c_str() : nullptr);
    _mqtt.setKeepAlive(15);
    _mqtt.setCleanSession(true);

    // LWT: {prefix}/system/status = "offline", QoS 1, retained
    {
        _lwtTopic = _buildTopic("system/status");
        _mqtt.setWill(_lwtTopic.c_str(), 1, true, "offline");
    }

    // ── Register AsyncMqttClient callbacks ────────────────────────────────────
    _mqtt.onConnect([this](bool sessionPresent) {
        _onConnect(sessionPresent);
    });

    _mqtt.onDisconnect([this](AsyncMqttClientDisconnectReason reason) {
        _onDisconnect(reason);
    });

    _mqtt.onMessage([this](char* topic,
                           char* payload,
                           AsyncMqttClientMessageProperties properties,
                           size_t len,
                           size_t index,
                           size_t total) {
        _onMessage(topic, payload, properties, len, index, total);
    });

    // ── Register RelayController state-change callback ────────────────────────
    // Every GPIO change auto-publishes the retained status topic.
    _relay->setOnStateChange([this](uint8_t ch, bool state) {
        publishRelayState(ch, state);
    });

    // ── First connect attempt (deferred until Wi-Fi is up) ────────────────────
    if (_enabled) {
        Serial.printf("[MQTT] Will connect to %s:%u when Wi-Fi is ready\n", _broker.c_str(), _port);
        _scheduleReconnect();
    }
}

void MqttManager::_loadConfig() {
    if (!_config) return;
    const JsonDocument& mDoc = _config->mqttConfig();
    _enabled  = mDoc["enabled"]  | false;
    _broker   = mDoc["broker"]   | "broker.emqx.io";
    _port     = mDoc["port"]     | static_cast<uint16_t>(1883);
    _username = "";
    _password = "";
    _prefix   = mDoc["prefix"]   | "elmahdy";
    _prefix.trim();
    _prefix.replace(" ", "");
    if (_prefix.length() == 0) {
        _prefix = "elmahdy";
    }
}

void MqttManager::_configureClient() {
    _mqtt.setServer(_broker.c_str(), _port);
    _mqtt.setClientId(_deviceId.c_str());
    _mqtt.setCredentials(
        _username.length() ? _username.c_str() : nullptr,
        _password.length() ? _password.c_str() : nullptr);
    _mqtt.setKeepAlive(15);
    _mqtt.setCleanSession(true);

    _lwtTopic = _buildTopic("system/status");
    _mqtt.setWill(_lwtTopic.c_str(), 1, true, "offline");
}

void MqttManager::reloadFromConfig() {
    if (!_config) return;

    _loadConfig();

    // Forcefully disconnect if currently connected
    if (_connected || _mqtt.connected()) {
        _mqtt.disconnect(true);
        _connected = false;
    }

    _reconnectPending = false;
    _backoffMs = MQTT_BACKOFF_MIN_MS;
    _configureClient();

    if (!_enabled) {
        Serial.println(F("[MQTT] Disabled in config"));
        return;
    }

    // Defer reconnect by 500 ms so the TCP stack fully tears down before
    // we initiate a new CONNECT to the (possibly new) broker.
    Serial.printf("[MQTT] Reconnecting to %s:%u in 500 ms\n",
                  _broker.c_str(), _port);
    _reconnectPending    = true;
    _backoffDeadlineMs   = millis() + 500UL;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * tick()
 * ───────────────────────────────────────────────────────────────────────────*/

void MqttManager::tick() {
    if (!_enabled) return;

    if (_reconnectPending && !_connected) {
        if (millis() >= _backoffDeadlineMs) {
            _reconnectPending = false;
            
            if (WiFi.isConnected()) {
                Serial.printf("[MQTT] Connecting to %s:%u (backoff %u ms)...\n",
                              _broker.c_str(), _port, _backoffMs);
                _mqtt.connect();
            } else {
                // Wi-Fi not ready, wait and try again
                _scheduleReconnect();
            }
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * isConnected()
 * ───────────────────────────────────────────────────────────────────────────*/

bool MqttManager::isConnected() const {
    return _connected;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * publishRelayState()
 * ───────────────────────────────────────────────────────────────────────────*/

void MqttManager::publishRelayState(uint8_t ch, bool state) {
    if (!_enabled || !_connected) return;

    // {prefix}/relay/{ch}/status
    char suffix[32];
    snprintf(suffix, sizeof(suffix), "relay/%u/status", static_cast<unsigned>(ch));
    String topic = _buildTopic(suffix);

    _mqtt.publish(topic.c_str(), 1, true, state ? "ON" : "OFF");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * publishAllStates()
 * ───────────────────────────────────────────────────────────────────────────*/

void MqttManager::publishAllStates() {
    if (!_enabled || !_connected) return;

    uint8_t count = _relay->getChannelCount();
    for (uint8_t i = 1; i <= count; ++i) {
        publishRelayState(i, _relay->getState(i));
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * setOnMessage()
 * ───────────────────────────────────────────────────────────────────────────*/

void MqttManager::setOnMessage(
    std::function<void(const char* topic, const char* payload)> cb)
{
    _onMessageCb = cb;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * _buildTopic()
 * ───────────────────────────────────────────────────────────────────────────*/

String MqttManager::_buildTopic(const char* suffix) const {
    // Reserve enough for "elmahdy/" (8) + suffix (up to ~64) + NUL
    String t;
    t.reserve(_prefix.length() + 1 + strlen(suffix) + 1);
    t  = _prefix;
    t += '/';
    t += suffix;
    return t;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * _publishHaDiscovery()  — T028
 *
 * For each configured relay channel, publishes:
 *   homeassistant/switch/{_deviceId}/relay_{ch}/config
 * with a full HA MQTT switch discovery JSON payload (retained, QoS 1).
 * ───────────────────────────────────────────────────────────────────────────*/

void MqttManager::_publishHaDiscovery() {
    if (!_enabled || !_connected) return;

    uint8_t count = _relay->getChannelCount();
    for (uint8_t ch = 1; ch <= count; ++ch) {
        // ── Build discovery topic ─────────────────────────────────────────────
        // homeassistant/switch/{device_id}/relay_{ch}/config
        char discoveryTopic[128];
        snprintf(discoveryTopic, sizeof(discoveryTopic),
                 "homeassistant/switch/%s/relay_%u/config",
                 _deviceId.c_str(), static_cast<unsigned>(ch));

        // ── Build state/command topic strings ────────────────────────────────
        char cmdSuffix[32];
        char statSuffix[32];
        snprintf(cmdSuffix,  sizeof(cmdSuffix),  "relay/%u/control",
                 static_cast<unsigned>(ch));
        snprintf(statSuffix, sizeof(statSuffix), "relay/%u/status",
                 static_cast<unsigned>(ch));
        String cmdTopic  = _buildTopic(cmdSuffix);
        String statTopic = _buildTopic(statSuffix);
        String availTopic = _buildTopic("system/status");

        // ── Build unique_id and friendly name ────────────────────────────────
        // unique_id: "elmahdy_relay_aabbccddeeff_1"
        char uniqueId[64];
        snprintf(uniqueId, sizeof(uniqueId), "%s_%u",
                 _deviceId.c_str(), static_cast<unsigned>(ch));

        // name: configured relay name, falling back to "Elmahdy Relay CH1"
        char friendlyName[64];
        const RelayChannel* channel = _relay ? _relay->getChannel(ch) : nullptr;
        if (channel && channel->name[0] != '\0') {
            snprintf(friendlyName, sizeof(friendlyName), "%s", channel->name);
        } else {
            snprintf(friendlyName, sizeof(friendlyName),
                     "Elmahdy Relay CH%u", static_cast<unsigned>(ch));
        }

        // ── Assemble JSON payload ─────────────────────────────────────────────
        // JsonDocument on the stack; serialise to String before leaving scope.
        JsonDocument doc;
        doc["name"]                  = friendlyName;
        doc["unique_id"]             = uniqueId;
        doc["command_topic"]         = cmdTopic;
        doc["state_topic"]           = statTopic;
        doc["payload_on"]            = "ON";
        doc["payload_off"]           = "OFF";
        doc["availability_topic"]    = availTopic;
        doc["payload_available"]     = "online";
        doc["payload_not_available"] = "offline";

        // device sub-object (shared across all channels of this device)
        {
            JsonObject device = doc["device"].to<JsonObject>();
            JsonArray identifiers = device["identifiers"].to<JsonArray>();
            identifiers.add(_deviceId);
            device["name"]         = "Elmahdy Relay";
            device["model"]        = "ESP8266-4CH";
            device["manufacturer"] = "Elmahdy";
            device["sw_version"]   = VERSION;
        }

        // Serialise to String
        String payload;
        payload.reserve(512);
        serializeJson(doc, payload);

        // Publish: retain=true, QoS=1
        _mqtt.publish(discoveryTopic, 1, true, payload.c_str());
    }

    Serial.printf("[MQTT] HA discovery published for %u channel(s)\n",
                  static_cast<unsigned>(_relay->getChannelCount()));
}

/* ─────────────────────────────────────────────────────────────────────────────
 * _scheduleReconnect()
 * ───────────────────────────────────────────────────────────────────────────*/

void MqttManager::_scheduleReconnect() {
    _reconnectPending    = true;
    _backoffDeadlineMs   = millis() + _backoffMs;

    Serial.printf("[MQTT] Reconnect scheduled in %u  ms\n", _backoffMs);

    // Double for next time, cap at maximum
    _backoffMs = (_backoffMs * 2 > MQTT_BACKOFF_MAX_MS)
                     ? MQTT_BACKOFF_MAX_MS
                     : _backoffMs * 2;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * _onConnect()
 * ───────────────────────────────────────────────────────────────────────────*/

void MqttManager::_onConnect(bool sessionPresent) {
    (void)sessionPresent;

    _connected        = true;
    _backoffMs        = MQTT_BACKOFF_MIN_MS;  // reset backoff counter
    _reconnectPending = false;

    Serial.println(F("[MQTT] Connected to broker"));

    // ── Publish online status (retained, QoS 1) ───────────────────────────────
    {
        String statusTopic = _buildTopic("system/status");
        _mqtt.publish(statusTopic.c_str(), 1, true, "online");
    }

    // ── Subscribe to all control topics (QoS 1) ───────────────────────────────
    // {prefix}/relay/{ch}/control  — per-channel; use wildcard + for all numbers
    {
        String t = _buildTopic("relay/+/control");
        _mqtt.subscribe(t.c_str(), 1);
    }
    // {prefix}/relay/all/control
    {
        String t = _buildTopic("relay/all/control");
        _mqtt.subscribe(t.c_str(), 1);
    }
    // {prefix}/scene/+/control  (T045 stub)
    {
        String t = _buildTopic("scene/+/control");
        _mqtt.subscribe(t.c_str(), 1);
    }
    // {prefix}/timer/+/control  (T032 stub)
    {
        String t = _buildTopic("timer/+/control");
        _mqtt.subscribe(t.c_str(), 1);
    }

    // ── Publish HA auto-discovery (T028) ──────────────────────────────────────
    _publishHaDiscovery();

    // ── Sync all relay states to broker ───────────────────────────────────────
    publishAllStates();
}

/* ─────────────────────────────────────────────────────────────────────────────
 * _onDisconnect()
 * ───────────────────────────────────────────────────────────────────────────*/

void MqttManager::_onDisconnect(AsyncMqttClientDisconnectReason reason) {
    _connected = false;
    Serial.printf("[MQTT] Disconnected (reason %d)\n",
                  static_cast<int>(reason));

    _scheduleReconnect();
}

/* ─────────────────────────────────────────────────────────────────────────────
 * _onMessage()
 *
 * AsyncMqttClient may deliver large payloads in multiple chunks
 * (index + len < total).  For control topics the payloads are tiny
 * ("ON"/"OFF"/"TOGGLE"/"CANCEL") so a single-chunk guard is sufficient.
 * Multi-chunk payloads are silently ignored.
 * ───────────────────────────────────────────────────────────────────────────*/

void MqttManager::_onMessage(char* topic,
                              char* payload,
                              AsyncMqttClientMessageProperties /*properties*/,
                              size_t len,
                              size_t index,
                              size_t total)
{
    // Only process complete, single-chunk messages
    if (index != 0 || len != total) return;

    // Null-terminate into a local buffer (payload is NOT guaranteed NUL-terminated
    // by AsyncMqttClient)
    char payloadBuf[32];
    size_t copyLen = (len < sizeof(payloadBuf) - 1) ? len : sizeof(payloadBuf) - 1;
    memcpy(payloadBuf, payload, copyLen);
    payloadBuf[copyLen] = '\0';

    // ── Optional passthrough ──────────────────────────────────────────────────
    if (_onMessageCb) {
        _onMessageCb(topic, payloadBuf);
    }

    // ── Parse prefix/ prefix to strip it ─────────────────────────────────────
    // Expected format: {prefix}/{sub-path}
    // We compare the first prefix.length()+1 bytes (including the slash).
    size_t prefixLen = _prefix.length();
    if (strncmp(topic, _prefix.c_str(), prefixLen) != 0 ||
        topic[prefixLen] != '/')
    {
        return;  // Not our prefix
    }
    const char* path = topic + prefixLen + 1;  // e.g. "relay/1/control"

    // ── relay/{ch}/control ────────────────────────────────────────────────────
    // path starts with "relay/" and ends with "/control"
    if (strncmp(path, "relay/", 6) == 0) {
        const char* chStr = path + 6;  // "1/control" or "all/control"

        if (strncmp(chStr, "all/control", 11) == 0) {
            // {prefix}/relay/all/control
            if (strcmp(payloadBuf, "ON") == 0) {
                _relay->setAll(true);
            } else if (strcmp(payloadBuf, "OFF") == 0) {
                _relay->setAll(false);
            }
        } else {
            // {prefix}/relay/{ch}/control — ch is a decimal number
            char* slash = strchr(chStr, '/');
            if (slash && strcmp(slash, "/control") == 0) {
                // Temporarily NUL-terminate to parse the channel number
                *slash = '\0';
                int ch = atoi(chStr);
                *slash = '/';

                if (ch >= 1 && ch <= static_cast<int>(_relay->getChannelCount())) {
                    uint8_t u = static_cast<uint8_t>(ch);
                    if (strcmp(payloadBuf, "ON") == 0) {
                        _relay->setState(u, true);
                    } else if (strcmp(payloadBuf, "OFF") == 0) {
                        _relay->setState(u, false);
                    } else if (strcmp(payloadBuf, "TOGGLE") == 0) {
                        _relay->toggle(u);
                    }
                }
            }
        }
        return;
    }

    // ── scene/{name}/control (T047) ──────────────────────────────────────────
    if (strncmp(path, "scene/", 6) == 0) {
        const char* nameStr = path + 6;
        char* slash = strchr(nameStr, '/');
        if (slash && strcmp(slash, "/control") == 0 &&
            strcmp(payloadBuf, "ON") == 0)
        {
            *slash = '\0';
            if (_sceneManager) {
                _sceneManager->activate(nameStr);
            }
            *slash = '/';
        }
        return;
    }

    // ── timer/{id}/control (T047) ─────────────────────────────────────────────
    if (strncmp(path, "timer/", 6) == 0) {
        const char* idStr = path + 6;
        char* slash = strchr(idStr, '/');
        if (slash && strcmp(slash, "/control") == 0 &&
            strcmp(payloadBuf, "CANCEL") == 0)
        {
            *slash = '\0';
            const uint16_t timerId = static_cast<uint16_t>(atoi(idStr));
            *slash = '/';
            if (_timerEngine && timerId > 0) {
                _timerEngine->cancel(timerId);
            }
        }
        return;
    }
}
