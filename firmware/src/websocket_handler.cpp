/*
 * websocket_handler.cpp — WebSocket server implementation
 *
 * See websocket_handler.h for the full protocol and design contract.
 *
 * Inbound message dispatch table
 * ────────────────────────────────
 *  "relay"    → handleRelay()    → relay_.setState() / toggle() → broadcastState()
 *  "relayAll" → handleRelayAll() → relay_.setAll()              → broadcastState()
 *  "scene"    → handleScene()    → scene_.activate()             → broadcastState()
 *  "getState" → handleGetState() → unicast state + info to caller only
 *
 * Connection lifecycle
 * ─────────────────────
 *  WS_EVT_CONNECT    → enforce MAX_WS_CLIENTS; send state + info to newcomer
 *  WS_EVT_DISCONNECT → no action needed beyond cleanupClients() in tick()
 *  WS_EVT_DATA       → accumulate fragments; dispatch when complete text frame
 *  WS_EVT_ERROR      → log and continue; ESPAsyncWebServer cleans up the slot
 *  WS_EVT_PONG       → ignored
 *
 * Memory discipline
 * ──────────────────
 * All JsonDocument objects are function-local; they are destroyed on return
 * and their heap is immediately reclaimed.  No persistent JSON allocations.
 *
 * Broadcast payloads are serialised into a WS_TX_BUF_SIZE (512) char array
 * on the stack.  serializeJson() null-terminates the buffer via the
 * snprintf-style overload.
 */

#include "websocket_handler.h"
#include "relay_controller.h"
#include "scene_manager.h"

#include <Arduino.h>
#include <ESP8266WiFi.h>    // WiFi.RSSI()

// ─── Static singleton back-pointer ───────────────────────────────────────────
WebSocketHandler* WebSocketHandler::instance_ = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

WebSocketHandler::WebSocketHandler()
    : ws_("/ws")
    , relay_(nullptr)
    , scene_(nullptr)
    , mqttConnected_(false)
    , _lastInfoMs_(0)
{
    // Store this instance for the static callback.  Only one
    // WebSocketHandler should exist for the lifetime of the firmware.
    instance_ = this;
}

// ─────────────────────────────────────────────────────────────────────────────
// begin()
// ─────────────────────────────────────────────────────────────────────────────

void WebSocketHandler::begin(AsyncWebServer& server,
                             RelayController& relay,
                             SceneManager& scene)
{
    relay_ = &relay;
    scene_ = &scene;

    ws_.onEvent(WebSocketHandler::onEvent);
    server.addHandler(&ws_);

    Serial.println(F("[WS] WebSocket handler registered on /ws"));
}

// ─────────────────────────────────────────────────────────────────────────────
// tick() — must be called from loop()
// ─────────────────────────────────────────────────────────────────────────────

void WebSocketHandler::tick()
{
    ws_.cleanupClients();

    // Periodic system-info broadcast every 5 seconds to all connected clients.
    uint32_t now = millis();
    if (ws_.count() > 0 && (now - _lastInfoMs_) >= 5000UL) {
        _lastInfoMs_ = now;
        broadcastInfo(mqttConnected_);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Static event callback (ESPAsyncWebServer API)
// ─────────────────────────────────────────────────────────────────────────────

/*static*/
void WebSocketHandler::onEvent(AsyncWebSocket*       server,
                               AsyncWebSocketClient* client,
                               AwsEventType          type,
                               void*                 arg,
                               uint8_t*              data,
                               size_t                len)
{
    // Guard against a call before begin() stores the instance pointer.
    if (instance_ == nullptr) return;

    switch (type) {
    case WS_EVT_CONNECT:
        instance_->handleConnect(client);
        break;

    case WS_EVT_DISCONNECT:
        instance_->handleDisconnect(client);
        break;

    case WS_EVT_DATA: {
        // ESPAsyncWebServer may deliver a single logical message split across
        // multiple WS_EVT_DATA calls.  AwsFrameInfo carries the fragment
        // metadata.
        const AwsFrameInfo* info = reinterpret_cast<const AwsFrameInfo*>(arg);

        // Only handle text frames; binary frames are unsupported.
        if (info->opcode != WS_TEXT) break;

        // Only dispatch on the final (or only) fragment of a message.
        if (info->final && info->index == 0 && info->len == static_cast<uint64_t>(len)) {
            // Single-chunk complete message — handle directly.
            instance_->handleMessage(client, data, len);
        }
        // Multi-chunk messages are not buffered; they are silently dropped.
        // The largest inbound message in this protocol is ~60 bytes; in
        // practice, fragmentation should never occur over a LAN.
        break;
    }

    case WS_EVT_ERROR:
        Serial.printf_P(PSTR("[WS] client #%u error(%u): %s\n"),
                        client->id(),
                        *reinterpret_cast<const uint16_t*>(arg),
                        reinterpret_cast<const char*>(data));
        break;

    case WS_EVT_PONG:
        // Nothing to do.
        break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// handleConnect()
// ─────────────────────────────────────────────────────────────────────────────

void WebSocketHandler::handleConnect(AsyncWebSocketClient* client)
{
    // ws_.count() already includes the newly-accepted client at this point.
    if (ws_.count() > MAX_WS_CLIENTS) {
        Serial.printf_P(PSTR("[WS] client #%u rejected — max %u clients reached\n"),
                        client->id(),
                        static_cast<unsigned>(MAX_WS_CLIENTS));
        client->close(1013 /*Try Again Later*/);
        return;
    }

    Serial.printf_P(PSTR("[WS] client #%u connected (%u/%u)\n"),
                    client->id(),
                    static_cast<unsigned>(ws_.count()),
                    static_cast<unsigned>(MAX_WS_CLIENTS));

    // Send initial state snapshot.
    char buf[WS_TX_BUF_SIZE];

    size_t stateLen = buildStateJson(buf, sizeof(buf));
    if (stateLen > 0) {
        client->text(buf, stateLen);
    }

    // Send initial system info.
    size_t infoLen = buildInfoJson(buf, sizeof(buf), mqttConnected_);
    if (infoLen > 0) {
        client->text(buf, infoLen);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// handleDisconnect()
// ─────────────────────────────────────────────────────────────────────────────

void WebSocketHandler::handleDisconnect(AsyncWebSocketClient* client)
{
    Serial.printf_P(PSTR("[WS] client #%u disconnected\n"), client->id());
    // ESPAsyncWebServer + cleanupClients() in tick() handle slot reclamation.
}

// ─────────────────────────────────────────────────────────────────────────────
// handleMessage() — parse JSON, dispatch by "type"
// ─────────────────────────────────────────────────────────────────────────────

void WebSocketHandler::handleMessage(AsyncWebSocketClient* client,
                                     const uint8_t*        data,
                                     size_t                len)
{
    if (relay_ == nullptr || scene_ == nullptr) {
        Serial.println(F("[WS] ERROR: controllers not initialised"));
        return;
    }

    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, data, len);
    if (err) {
        Serial.printf_P(PSTR("[WS] client #%u JSON parse error: %s\n"),
                        client->id(), err.c_str());
        return;
    }

    const char* type = doc["type"] | "";

    if (strcmp(type, "relay") == 0) {
        handleRelay(client, doc);
    } else if (strcmp(type, "relayAll") == 0) {
        handleRelayAll(client, doc);
    } else if (strcmp(type, "scene") == 0) {
        handleScene(client, doc);
    } else if (strcmp(type, "getState") == 0) {
        handleGetState(client);
    } else {
        Serial.printf_P(PSTR("[WS] client #%u unknown type: %s\n"),
                        client->id(), type);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Command handlers
// ─────────────────────────────────────────────────────────────────────────────

void WebSocketHandler::handleRelay(AsyncWebSocketClient* client,
                                   const JsonDocument&   doc)
{
    // {"type":"relay","id":1,"action":"on"|"off"|"toggle"}
    if (!doc["id"].is<uint8_t>()) {
        Serial.printf_P(PSTR("[WS] client #%u relay: missing/invalid 'id'\n"),
                        client->id());
        return;
    }
    const uint8_t id     = doc["id"].as<uint8_t>();
    const char*   action = doc["action"] | "";

    if (strcmp(action, "on") == 0) {
        relay_->setState(id, true);
    } else if (strcmp(action, "off") == 0) {
        relay_->setState(id, false);
    } else if (strcmp(action, "toggle") == 0) {
        relay_->toggle(id);
    }
}

void WebSocketHandler::handleRelayAll(AsyncWebSocketClient* client,
                                      const JsonDocument&   doc)
{
    // {"type":"relayAll","action":"on"|"off"}
    const char* action = doc["action"] | "";

    if (strcmp(action, "on") == 0) {
        relay_->setAll(true);
    } else if (strcmp(action, "off") == 0) {
        relay_->setAll(false);
    }
}

void WebSocketHandler::handleScene(AsyncWebSocketClient* client,
                                   const JsonDocument&   doc)
{
    const char* name = doc["name"] | "";
    if (name[0] == '\0') {
        Serial.printf_P(PSTR("[WS] client #%u scene: missing 'name'\n"),
                        client->id());
        return;
    }

    if (!scene_->activate(name)) {
        Serial.printf_P(PSTR("[WS] client #%u scene: '%s' not found\n"),
                        client->id(), name);
    }
}

void WebSocketHandler::handleGetState(AsyncWebSocketClient* client)
{
    // Unicast — respond only to the requesting client.
    char buf[WS_TX_BUF_SIZE];

    const size_t stateLen = buildStateJson(buf, sizeof(buf));
    if (stateLen > 0) {
        client->text(buf, stateLen);
    }

    const size_t infoLen = buildInfoJson(buf, sizeof(buf), mqttConnected_);
    if (infoLen > 0) {
        client->text(buf, infoLen);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// broadcastState()
// ─────────────────────────────────────────────────────────────────────────────

void WebSocketHandler::broadcastState()
{
    if (ws_.count() == 0) return;

    char buf[WS_TX_BUF_SIZE];
    const size_t len = buildStateJson(buf, sizeof(buf));
    if (len > 0) {
        ws_.textAll(buf, len);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// broadcastTimerUpdate()
// ─────────────────────────────────────────────────────────────────────────────

void WebSocketHandler::broadcastTimerUpdate(const JsonArray& timers)
{
    if (ws_.count() == 0) return;

    // Build {"type":"timer","timers":[…]} around the caller-supplied array.
    JsonDocument doc;
    doc["type"]   = "timer";
    doc["timers"] = timers;  // copies the array reference into the new doc

    char buf[WS_TX_BUF_SIZE];
    const size_t len = serializeJson(doc, buf, sizeof(buf));
    if (len > 0 && len < sizeof(buf)) {
        ws_.textAll(buf, len);
    } else {
        Serial.println(F("[WS] broadcastTimerUpdate: buffer overflow"));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// broadcastInfo()
// ─────────────────────────────────────────────────────────────────────────────

void WebSocketHandler::broadcastInfo(bool mqttConnected)
{
    // Cache the MQTT state so handleConnect() can include it in the
    // initial info message sent to new clients.
    mqttConnected_ = mqttConnected;

    if (ws_.count() == 0) return;

    char buf[WS_TX_BUF_SIZE];
    const size_t len = buildInfoJson(buf, sizeof(buf), mqttConnected);
    if (len > 0) {
        ws_.textAll(buf, len);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// broadcastConfigChanged()
// ─────────────────────────────────────────────────────────────────────────────

void WebSocketHandler::broadcastConfigChanged(const char* section)
{
    if (ws_.count() == 0) return;

    JsonDocument doc;
    doc["type"]    = "configChanged";
    doc["section"] = section;

    char buf[WS_TX_BUF_SIZE];
    const size_t len = serializeJson(doc, buf, sizeof(buf));
    if (len > 0 && len < sizeof(buf)) {
        ws_.textAll(buf, len);
    } else {
        Serial.println(F("[WS] broadcastConfigChanged: buffer overflow"));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// sendToClient() — unicast helper
// ─────────────────────────────────────────────────────────────────────────────

void WebSocketHandler::sendToClient(AsyncWebSocketClient* client,
                                    const char*           payload)
{
    client->text(payload);
}

// ─────────────────────────────────────────────────────────────────────────────
// buildStateJson() — fill buf with the current relay-state message
// ─────────────────────────────────────────────────────────────────────────────

size_t WebSocketHandler::buildStateJson(char* buf, size_t bufLen)
{
    // {"type":"state","relays":[{"id":1,"state":true},…]}
    JsonDocument doc;
    doc["type"] = "state";

    JsonArray relays = doc["relays"].to<JsonArray>();

    const uint8_t count = relay_->getChannelCount();
    for (uint8_t i = 1; i <= count; ++i) {
        const RelayChannel* ch = relay_->getChannel(i);
        JsonObject entry = relays.add<JsonObject>();
        entry["id"]    = i;
        entry["name"]  = ch ? ch->name : "";
        entry["state"] = relay_->getState(i);
    }

    const size_t len = serializeJson(doc, buf, bufLen);
    if (len >= bufLen) {
        // serializeJson truncated — the buffer is too small.
        Serial.println(F("[WS] buildStateJson: buffer overflow"));
        return 0;
    }
    return len;
}

// ─────────────────────────────────────────────────────────────────────────────
// buildInfoJson() — fill buf with the system-info message
// ─────────────────────────────────────────────────────────────────────────────

size_t WebSocketHandler::buildInfoJson(char* buf, size_t bufLen,
                                       bool mqttConnected)
{
    JsonDocument doc;
    doc["type"]    = "info";
    doc["rssi"]    = static_cast<int32_t>(WiFi.RSSI());
    doc["uptime"]  = static_cast<uint32_t>(millis() / 1000UL);
    doc["mqtt"]    = mqttConnected;
    doc["heap"]    = static_cast<uint32_t>(ESP.getFreeHeap());
    doc["version"] = F(VERSION);
    doc["mac"]     = WiFi.macAddress();
    doc["ip"]      = WiFi.localIP().toString();

    {
        const time_t now = time(nullptr);
        doc["ntpSynced"] = (now >= 1577836800L);
        if (now >= 1577836800L) {
            char timeBuf[20];
            struct tm* ti = localtime(&now);
            int h = ti->tm_hour % 12;
            if (h == 0) h = 12;
            snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d %s", h, ti->tm_min, ti->tm_hour >= 12 ? "PM" : "AM");
            doc["time"] = timeBuf;
        } else {
            doc["time"] = "";
        }
    }

    const size_t len = serializeJson(doc, buf, bufLen);
    if (len >= bufLen) {
        Serial.println(F("[WS] buildInfoJson: buffer overflow"));
        return 0;
    }
    return len;
}
