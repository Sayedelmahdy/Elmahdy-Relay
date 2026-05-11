/*
 * websocket_handler.h — WebSocket server over ESPAsyncWebServer
 *
 * Endpoint : ws://{device-ip}/ws
 * Max clients : MAX_WS_CLIENTS (4) concurrent connections
 * Protocol  : JSON text frames; all messages carry a "type" field
 *
 * Client → Server types : relay | relayAll | scene | getState
 * Server → Client types : state | timer | info | configChanged
 *
 * Design notes
 * ─────────────
 * • RelayController and SceneManager are forward-declared here so that
 *   this header does not pull in their full definitions, preventing
 *   circular-include chains.  The full headers are included in the .cpp.
 * • The AsyncWebSocket object is a direct member (not a pointer) so that
 *   no heap allocation is needed at initialisation time.
 * • All broadcast helpers serialise into a fixed 512-byte stack buffer;
 *   that ceiling comfortably covers the largest legal JSON message given
 *   MAX_CHANNELS = 4 and MAX_TIMERS = 8.
 * • No delay() calls anywhere — the module is entirely non-blocking.
 *
 * Target: ESP8266 (ESP-12F), Arduino Core 3.x, C++17, PlatformIO
 */

#ifndef WEBSOCKET_HANDLER_H_
#define WEBSOCKET_HANDLER_H_

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

#include "config.h"   // MAX_WS_CLIENTS, MAX_CHANNELS, MAX_TIMERS

// Forward declarations — full definitions live in their respective .h files.
class RelayController;
class SceneManager;

// ─────────────────────────────────────────────────────────────────────────────
// Broadcast buffer sizing
//
// Largest broadcast is the "state" message with MAX_CHANNELS relay objects:
//   {"type":"state","relays":[{"id":1,"state":true},…]}
// 64 bytes base + 28 bytes × MAX_CHANNELS ≈ 176 bytes.
//
// "timer" with MAX_TIMERS entries:
//   {"type":"timer","timers":[{"id":1,"remaining":4294967295},…]}
// 64 bytes base + 36 bytes × MAX_TIMERS ≈ 352 bytes.
//
// 512 bytes is a conservative ceiling with room for future fields.
// ─────────────────────────────────────────────────────────────────────────────
static constexpr size_t WS_TX_BUF_SIZE = 512;

class WebSocketHandler {
public:
    WebSocketHandler();

    // Register the /ws endpoint on server; store references to the two
    // controllers that command dispatch requires.
    void begin(AsyncWebServer& server,
               RelayController& relay,
               SceneManager& scene);

    // ── Outbound broadcasts ────────────────────────────────────────────────

    // Build {"type":"state","relays":[…]} from all relay channels and
    // deliver to every connected client.
    void broadcastState();

    // Build {"type":"timer","timers":[…]} from the supplied JsonArray and
    // deliver to every connected client.
    // The caller owns the JsonDocument that contains `timers`; this method
    // serialises the array before the call returns so the caller may free
    // the document immediately afterwards.
    void broadcastTimerUpdate(const JsonArray& timers);

    // Build {"type":"info",…} from live system counters (WiFi RSSI,
    // millis() uptime, MQTT connection flag, ESP free heap) and deliver
    // to every connected client.
    void broadcastInfo(bool mqttConnected);

    // Build {"type":"configChanged","section":…} and deliver to every
    // connected client.
    void broadcastConfigChanged(const char* section);

    // Update the cached MQTT connection state for periodic broadcasts
    void setMqttConnected(bool connected) { mqttConnected_ = connected; }

    // ── Maintenance ────────────────────────────────────────────────────────

    // Must be called from loop().  Invokes ws_.cleanupClients() to reclaim
    // slots occupied by disconnected clients.
    void tick();

    // Expose the underlying AsyncWebSocket so that other modules (e.g.
    // OtaHandler) can call ws_.count() without coupling to this class.
    AsyncWebSocket& socket() { return ws_; }

private:
    // ── Internal event dispatch ────────────────────────────────────────────

    // Raw ESPAsyncWebServer event callback; dispatches to the handle*
    // methods below based on AwsEventType.
    static void onEvent(AsyncWebSocket* server,
                        AsyncWebSocketClient* client,
                        AwsEventType type,
                        void* arg,
                        uint8_t* data,
                        size_t len);

    // Called on WS_EVT_CONNECT.  Enforces MAX_WS_CLIENTS limit; sends the
    // initial state + info messages to the new client.
    void handleConnect(AsyncWebSocketClient* client);

    // Called on WS_EVT_DISCONNECT.
    void handleDisconnect(AsyncWebSocketClient* client);

    // Called on WS_EVT_DATA for complete text frames.  Parses JSON and
    // dispatches to the appropriate relay / scene command.
    void handleMessage(AsyncWebSocketClient* client,
                       const uint8_t* data,
                       size_t len);

    // ── Command handlers ──────────────────────────────────────────────────

    void handleRelay(AsyncWebSocketClient* client, const JsonDocument& doc);
    void handleRelayAll(AsyncWebSocketClient* client, const JsonDocument& doc);
    void handleScene(AsyncWebSocketClient* client, const JsonDocument& doc);
    void handleGetState(AsyncWebSocketClient* client);

    // ── Unicast helper ─────────────────────────────────────────────────────

    // Send the contents of buf (null-terminated) to a single client.
    void sendToClient(AsyncWebSocketClient* client, const char* payload);

    // ── State helpers ──────────────────────────────────────────────────────

    // Populate a JSON document with the current relay state array and
    // serialise it into buf.  Returns number of bytes written (excl. NUL).
    size_t buildStateJson(char* buf, size_t bufLen);

    // Serialise a {"type":"info",…} message into buf.
    size_t buildInfoJson(char* buf, size_t bufLen, bool mqttConnected);

    // ── Members ───────────────────────────────────────────────────────────

    AsyncWebSocket    ws_;               // owns the /ws endpoint
    RelayController*  relay_;            // non-owning; set by begin()
    SceneManager*     scene_;            // non-owning; set by begin()
    bool              mqttConnected_;    // last known MQTT state for info msgs
    uint32_t          _lastInfoMs_;      // millis() of last periodic info broadcast

    // Static back-pointer used by the static onEvent callback to reach
    // the owning WebSocketHandler instance.
    static WebSocketHandler* instance_;
};

#endif /* WEBSOCKET_HANDLER_H_ */
