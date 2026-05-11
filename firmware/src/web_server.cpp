/*
 * web_server.cpp — ESPAsyncWebServer: static files, CORS, REST stubs, SPA fallback.
 */

#include "web_server.h"
#include "config_manager.h"
#include "relay_controller.h"
#include "websocket_handler.h"
#include "wifi_manager.h"
#include "timer_engine.h"
#include "scene_manager.h"
#include "buzzer_controller.h"
#include "mqtt_manager.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Ticker.h>
#include <string.h>
// Update (ESP8266 Updater) is part of the core — no header needed

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

static Ticker       _restartTicker;
static bool         _otaError = false;

// Deferred WiFi reconnect — lets the HTTP response flush before we drop STA.
static Ticker       _wifiReconnectTicker;
static WiFiManager* _wifiMgrRef        = nullptr;
static String       _pendingConnSsid;
static String       _pendingConnPass;

// Minimal percent-decoder for URL path segments.
static String urlDecode(const String& s) {
    String out;
    out.reserve(s.length());
    for (unsigned i = 0; i < s.length(); ++i) {
        if (s[i] == '%' && i + 2 < s.length()) {
            const char hi = s[i + 1], lo = s[i + 2];
            auto h = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return -1;
            };
            if (h(hi) >= 0 && h(lo) >= 0) {
                out += (char)((h(hi) << 4) | h(lo));
                i += 2;
                continue;
            }
        } else if (s[i] == '+') {
            out += ' ';
            continue;
        }
        out += s[i];
    }
    return out;
}

static String sanitizeHostname(const char* raw) {
    String out;
    const char* p = raw ? raw : "";
    out.reserve(20);
    for (; *p && out.length() < 20; ++p) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out += c;
        } else if ((c == '-' || c == '_' || c == ' ') && out.length() > 0) {
            if (out[out.length() - 1] != '-') out += '-';
        }
    }
    while (out.length() > 0 && out[out.length() - 1] == '-') {
        out.remove(out.length() - 1);
    }
    if (out.length() == 0) {
        out = F("elmahdyrelay");
    }
    return out;
}

WebServer::WebServer()
    : _config(nullptr)
    , _relay(nullptr)
    , _ws(nullptr)
    , _wifi(nullptr)
    , _timerEngine(nullptr)
    , _sceneManager(nullptr)
    , _mqttManager(nullptr)
    , _server(nullptr)
{}

void WebServer::setWifiManager(WiFiManager& wifi) {
    _wifi = &wifi;
}

void WebServer::setTimerEngine(TimerEngine& timer) {
    _timerEngine = &timer;
}

void WebServer::setSceneManager(SceneManager& scene) {
    _sceneManager = &scene;
}

void WebServer::setMqttManager(MqttManager& mqtt) {
    _mqttManager = &mqtt;
}

// ─────────────────────────────────────────────────────────────────────────────
// begin()
// ─────────────────────────────────────────────────────────────────────────────

void WebServer::begin(ConfigManager&    config,
                      RelayController&  relay,
                      WebSocketHandler& ws,
                      SceneManager&     scene)
{
    _config = &config;
    _relay  = &relay;
    _ws     = &ws;

    _server = new AsyncWebServer(80);

    // Attach WebSocket endpoint (/ws) to our server instance.
    ws.begin(*_server, relay, scene);

    registerRoutes();

    _server->begin();
    Serial.println(F("[WebServer] Listening on port 80"));
}

// ─────────────────────────────────────────────────────────────────────────────
// server()
// ─────────────────────────────────────────────────────────────────────────────

AsyncWebServer& WebServer::server() {
    return *_server;
}

// ─────────────────────────────────────────────────────────────────────────────
// Private helpers
// ─────────────────────────────────────────────────────────────────────────────

void WebServer::addCorsHeaders(AsyncWebServerResponse* response) {
    response->addHeader("Access-Control-Allow-Origin",  "*");
    response->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    response->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
}

const char* WebServer::mimeForPath(const String& path) {
    // Strip .gz suffix before checking extension
    String p = path;
    if (p.endsWith(".gz")) {
        p = p.substring(0, p.length() - 3);
    }
    int dot = p.lastIndexOf('.');
    if (dot < 0) return "application/octet-stream";
    String ext = p.substring(dot + 1);
    ext.toLowerCase();
    for (size_t i = 0; i < MIME_TABLE_SIZE; i++) {
        if (ext == MIME_TABLE[i].ext) return MIME_TABLE[i].mime;
    }
    return "application/octet-stream";
}

bool WebServer::serveFileWithGzip(AsyncWebServerRequest* request,
                                  const String&          fsPath,
                                  const char*            mimeType)
{
    // Try .gz variant first
    String gzPath = fsPath + ".gz";
    if (LittleFS.exists(gzPath)) {
        AsyncWebServerResponse* resp =
            request->beginResponse(LittleFS, gzPath, mimeType);
        resp->addHeader("Content-Encoding", "gzip");
        addCorsHeaders(resp);
        request->send(resp);
        return true;
    }
    if (LittleFS.exists(fsPath)) {
        AsyncWebServerResponse* resp =
            request->beginResponse(LittleFS, fsPath, mimeType);
        addCorsHeaders(resp);
        request->send(resp);
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// registerRoutes()
// ─────────────────────────────────────────────────────────────────────────────

void WebServer::registerRoutes() {

    // ── OPTIONS preflight — respond 200 + CORS to every /api/* path ──────────
    _server->on("/api/*", HTTP_OPTIONS,
        [](AsyncWebServerRequest* request) {
            AsyncWebServerResponse* resp = request->beginResponse(200, "text/plain", "");
            addCorsHeaders(resp);
            request->send(resp);
        }
    );

    // ── GET /api/info ─────────────────────────────────────────────────────────
    _server->on("/api/info", HTTP_GET,
        [this](AsyncWebServerRequest* request) {
            JsonDocument doc;
            doc["name"]     = "Elmahdy Relay";
            doc["version"]  = VERSION;
            doc["mac"]      = WiFi.macAddress();
            doc["ip"]       = (_wifi && _wifi->isConnected())
                ? _wifi->getLocalIp().toString()
                : String("");
            doc["freeHeap"] = static_cast<uint32_t>(ESP.getFreeHeap());

            const JsonDocument& sysDoc = _config->systemConfig();
            const char* hostname = sysDoc["hostname"] | "elmahdyrelay";
            doc["hostname"] = hostname;

            JsonObject mdns = doc["mdns"].to<JsonObject>();
            const bool mdnsEnabled = _wifi ? _wifi->isMdnsEnabled()
                                           : (sysDoc["mdnsEnabled"] | true);
            mdns["enabled"]  = mdnsEnabled;
            mdns["running"]  = _wifi ? _wifi->isMdnsRunning() : false;
            mdns["hostname"] = hostname;
            mdns["url"]      = mdnsEnabled
                ? (_wifi ? _wifi->getMdnsUrl()
                         : String(F("http://")) + hostname + F(".local"))
                : String("");

            // Channel count from in-memory relay config
            const JsonDocument& relayDoc = _config->relayConfig();
            doc["channels"] = relayDoc["channelCount"] | 2;

            JsonArray ep = doc["endpoints"].to<JsonArray>();
            ep.add("/api/status");
            ep.add("/api/relay");
            ep.add("/api/config");
            ep.add("/api/timers");
            ep.add("/api/scenes");
            ep.add("/api/system");

            String body;
            body.reserve(256);
            serializeJson(doc, body);

            AsyncWebServerResponse* resp =
                request->beginResponse(200, "application/json", body);
            addCorsHeaders(resp);
            request->send(resp);
        }
    );

    // ── GET /api/wifi/scan — fire async scan, return immediately ─────────────
    // Client must then poll GET /api/wifi/results until "scanning" is false.
    _server->on("/api/wifi/scan", HTTP_GET,
        [this](AsyncWebServerRequest* request) {
            if (_wifi) _wifi->scan();
            AsyncWebServerResponse* resp =
                request->beginResponse(200, "application/json",
                                       F("{\"scanning\":true}"));
            addCorsHeaders(resp);
            request->send(resp);
        }
    );

    // ── GET /api/wifi/results — poll for async scan completion ───────────────
    _server->on("/api/wifi/results", HTTP_GET,
        [this](AsyncWebServerRequest* request) {
            String body;
            if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
                body = F("{\"scanning\":true}");
            } else {
                String nets = _wifi ? _wifi->getScanResults() : F("[]");
                body = "{\"scanning\":false,\"networks\":" + nets + "}";
            }
            AsyncWebServerResponse* resp =
                request->beginResponse(200, "application/json", body);
            addCorsHeaders(resp);
            request->send(resp);
        }
    );

    // ── GET /api/config/wifi ──────────────────────────────────────────────────
    _server->on("/api/config/wifi", HTTP_GET,
        [this](AsyncWebServerRequest* request) {
            // Return wifi config minus password for security
            const JsonDocument& wDoc = _config->wifiConfig();
            JsonDocument out;
            out["ssid"]       = wDoc["ssid"]       | "";
            out["hasPassword"] = strlen(wDoc["password"] | "") > 0;
            out["dhcp"]       = wDoc["dhcp"]       | true;
            out["staticIp"]   = wDoc["staticIp"]   | "192.168.1.50";
            out["gateway"]    = wDoc["gateway"]    | "192.168.1.1";
            out["subnet"]     = wDoc["subnet"]     | "255.255.255.0";
            out["dns"]        = wDoc["dns"]        | "8.8.8.8";
            out["apSsid"]     = wDoc["apSsid"]     | "";
            out["apPassword"] = wDoc["apPassword"] | "12345678";
            String body;
            serializeJson(out, body);
            AsyncWebServerResponse* resp =
                request->beginResponse(200, "application/json", body);
            addCorsHeaders(resp);
            request->send(resp);
        }
    );

    // ── POST /api/config/wifi — save credentials and trigger STA connect ──────
    // Body is accumulated in _tempObject (String*) by the body handler, then
    // parsed and freed in the request handler. This is the canonical pattern
    // for JSON POST with ESPAsyncWebServer.
    _server->on("/api/config/wifi", HTTP_POST,
        [this](AsyncWebServerRequest* request) {
            String* bodyStr = reinterpret_cast<String*>(request->_tempObject);
            if (!bodyStr) {
                request->send(400, "application/json", "{\"error\":\"No body\"}");
                return;
            }
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, *bodyStr);
            delete bodyStr;
            request->_tempObject = nullptr;

            if (err) {
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }
            const char* ssid = doc["ssid"] | "";
            const char* incomingPass = doc["password"] | "";
            const char* passwordMode = doc["passwordMode"] | "";

            const bool useDhcp = doc["dhcp"] | true;
            if (!useDhcp) {
                IPAddress ip, gw, sn, dns;
                if (!ip.fromString(doc["staticIp"] | "") ||
                    !gw.fromString(doc["gateway"] | "") ||
                    !sn.fromString(doc["subnet"] | "") ||
                    !dns.fromString(doc["dns"] | "8.8.8.8"))
                {
                    AsyncWebServerResponse* resp =
                        request->beginResponse(400, "application/json",
                                               "{\"error\":\"invalid static network settings\"}");
                    addCorsHeaders(resp);
                    request->send(resp);
                    return;
                }
            }

            // Persist credentials. For the same SSID, an empty password means
            // "keep the existing password" so IP edits cannot wipe WiFi. For
            // a new SSID, empty password is valid for an open network.
            JsonDocument& wDoc = _config->wifiConfigMut();
            const char* savedSsid = wDoc["ssid"] | "";
            const char* savedPass = wDoc["password"] | "";
            const bool sameSsid = strcmp(ssid, savedSsid) == 0;
            const bool keepSavedPassword = strcmp(passwordMode, "keep") == 0 && sameSsid;
            const bool forceOpenNetwork = strcmp(passwordMode, "open") == 0;
            const char* pass = "";
            if (keepSavedPassword) {
                pass = savedPass;
            } else if (forceOpenNetwork) {
                pass = "";
            } else if (incomingPass[0] != '\0') {
                pass = incomingPass;
            } else if (sameSsid) {
                pass = savedPass;
            }
            wDoc["ssid"]       = ssid;
            wDoc["password"]   = pass;
            wDoc["dhcp"]       = useDhcp;
            wDoc["staticIp"]   = doc["staticIp"] | "192.168.1.50";
            wDoc["gateway"]    = doc["gateway"]  | "192.168.1.1";
            wDoc["subnet"]     = doc["subnet"]   | "255.255.255.0";
            wDoc["dns"]        = doc["dns"]      | "8.8.8.8";
            if (doc["apSsid"].is<const char*>()) {
                wDoc["apSsid"] = doc["apSsid"].as<const char*>();
            }
            if (doc["apPassword"].is<const char*>()) {
                wDoc["apPassword"] = doc["apPassword"].as<const char*>();
            }
            _config->saveWifi();

            // Send response FIRST so the browser receives it before we
            // disconnect the STA interface (which would drop the TCP session).
            AsyncWebServerResponse* resp =
                request->beginResponse(200, "application/json",
                                       "{\"success\":true}");
            addCorsHeaders(resp);
            request->send(resp);

            // Schedule connect 800 ms later — enough time for the response
            // to be ACKed by the client over the existing STA connection.
            if (_wifi) {
                _wifiMgrRef    = _wifi;
                _pendingConnSsid = String(ssid);
                _pendingConnPass = String(pass);
                _wifiReconnectTicker.once_ms(800, []() {
                    if (_wifiMgrRef) {
                        _wifiMgrRef->connectSTA(_pendingConnSsid.c_str(),
                                                _pendingConnPass.c_str());
                    }
                });
            }
        },
        nullptr,
        [](AsyncWebServerRequest* request,
           uint8_t* data, size_t len,
           size_t index, size_t total)
        {
            if (index == 0) {
                request->_tempObject = new String();
                reinterpret_cast<String*>(request->_tempObject)->reserve(total);
            }
            reinterpret_cast<String*>(request->_tempObject)
                ->concat(reinterpret_cast<const char*>(data), len);
        }
    );

    // ── GET /api/status — relay states + system info ─────────────────────────
    _server->on("/api/status", HTTP_GET,
        [this](AsyncWebServerRequest* request) {
            JsonDocument doc;

            // Relay array
            JsonArray relays = doc["relays"].to<JsonArray>();
            uint8_t count = _relay->getChannelCount();
            for (uint8_t i = 1; i <= count; i++) {
                const RelayChannel* ch = _relay->getChannel(i);
                if (!ch) continue;
                JsonObject r = relays.add<JsonObject>();
                r["id"]    = ch->id;
                r["name"]  = ch->name;
                r["state"] = ch->state;
                r["pulse"] = (ch->pulseEndTime != 0);
            }

            // WiFi object
            JsonObject wifi = doc["wifi"].to<JsonObject>();
            if (_wifi && _wifi->isConnected()) {
                wifi["ssid"] = WiFi.SSID();
                wifi["rssi"] = _wifi->getRssi();
                wifi["ip"]   = _wifi->getLocalIp().toString();
            } else {
                const JsonDocument& wDoc = _config->wifiConfig();
                wifi["ssid"] = wDoc["ssid"] | "";
                wifi["rssi"] = 0;
                wifi["ip"]   = "";
            }
            {
                const JsonDocument& wDoc = _config->wifiConfig();
                wifi["dhcp"]     = wDoc["dhcp"]     | true;
                wifi["staticIp"] = wDoc["staticIp"] | "192.168.1.50";
                wifi["gateway"]  = wDoc["gateway"]  | "192.168.1.1";
                wifi["subnet"]   = wDoc["subnet"]   | "255.255.255.0";
                wifi["dns"]      = wDoc["dns"]      | "8.8.8.8";
            }

            // MQTT object
            JsonObject mqtt = doc["mqtt"].to<JsonObject>();
            const JsonDocument& mDoc = _config->mqttConfig();
            mqtt["enabled"] = _mqttManager ? _mqttManager->isEnabled() : (mDoc["enabled"] | false);
            mqtt["connected"] = _mqttManager ? _mqttManager->isConnected() : false;
            mqtt["broker"] = mDoc["broker"] | "";
            mqtt["port"] = mDoc["port"] | 1883;
            mqtt["prefix"] = mDoc["prefix"] | "elmahdy";

            // System fields
            doc["uptime"]    = millis() / 1000UL;
            doc["version"]   = VERSION;
            const JsonDocument& sysDoc = _config->systemConfig();
            const char* hostname = sysDoc["hostname"] | "elmahdyrelay";
            doc["hostname"]  = hostname;
            JsonObject mdns = doc["mdns"].to<JsonObject>();
            const bool mdnsEnabled = _wifi ? _wifi->isMdnsEnabled()
                                           : (sysDoc["mdnsEnabled"] | true);
            mdns["enabled"]  = mdnsEnabled;
            mdns["running"]  = _wifi ? _wifi->isMdnsRunning() : false;
            mdns["hostname"] = hostname;
            mdns["url"]      = mdnsEnabled
                ? (_wifi ? _wifi->getMdnsUrl()
                         : String(F("http://")) + hostname + F(".local"))
                : String("");
            {
                const time_t now = time(nullptr);
                doc["ntpSynced"] = (now >= 1577836800L);
                if (now >= 1577836800L) {
                    char timeBuf[20];
                    struct tm* ti = localtime(&now);
                    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d %s",
                             (ti->tm_hour % 12 == 0 ? 12 : ti->tm_hour % 12), ti->tm_min, ti->tm_hour >= 12 ? "PM" : "AM");
                    doc["time"] = timeBuf;
                } else {
                    doc["time"] = "";
                }
            }

            String body;
            body.reserve(512);
            serializeJson(doc, body);

            AsyncWebServerResponse* resp =
                request->beginResponse(200, "application/json", body);
            addCorsHeaders(resp);
            request->send(resp);
        }
    );

    // ── POST /api/relay/all — set all relays to one state ────────────────────
    // Must be registered BEFORE the wildcard /api/relay/{ch} route so that
    // ESPAsyncWebServer's first-match wins and "all" is never misread as
    // a channel number.
    _server->on("/api/relay/all", HTTP_POST,
        [this](AsyncWebServerRequest* request) {
            String* bodyStr = reinterpret_cast<String*>(request->_tempObject);
            if (!bodyStr) {
                request->send(400, "application/json", "{\"error\":\"No body\"}");
                return;
            }
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, *bodyStr);
            delete bodyStr;
            request->_tempObject = nullptr;

            if (err) {
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }

            const char* action = doc["action"] | "";
            if (strcmp(action, "on") == 0) {
                _relay->setAll(true);
            } else if (strcmp(action, "off") == 0) {
                _relay->setAll(false);
            } else {
                request->send(400, "application/json", "{\"error\":\"Invalid action\"}");
                return;
            }

            // Broadcast updated state to all WebSocket clients
            _ws->broadcastState();

            // Build response: array of all channel id + new state
            JsonDocument resp_doc;
            JsonArray arr = resp_doc["relays"].to<JsonArray>();
            uint8_t count = _relay->getChannelCount();
            for (uint8_t i = 1; i <= count; i++) {
                JsonObject r = arr.add<JsonObject>();
                r["id"]    = i;
                r["state"] = _relay->getState(i);
            }
            String body;
            body.reserve(128);
            serializeJson(resp_doc, body);

            AsyncWebServerResponse* resp =
                request->beginResponse(200, "application/json", body);
            addCorsHeaders(resp);
            request->send(resp);
        },
        nullptr,
        [](AsyncWebServerRequest* request,
           uint8_t* data, size_t len,
           size_t index, size_t total)
        {
            if (index == 0) {
                request->_tempObject = new String();
                reinterpret_cast<String*>(request->_tempObject)->reserve(total);
            }
            reinterpret_cast<String*>(request->_tempObject)
                ->concat(reinterpret_cast<const char*>(data), len);
        }
    );

    // ── POST /api/relay/{ch} — control a single relay ─────────────────────
    _server->on("/api/relay/*", HTTP_POST,
        [this](AsyncWebServerRequest* request) {
            String* bodyStr = reinterpret_cast<String*>(request->_tempObject);
            if (!bodyStr) {
                request->send(400, "application/json", "{\"error\":\"No body\"}");
                return;
            }
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, *bodyStr);
            delete bodyStr;
            request->_tempObject = nullptr;

            if (err) {
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }

            // Extract channel number from URL: /api/relay/N
            const String& url = request->url();
            int ch = url.substring(url.lastIndexOf('/') + 1).toInt();
            if (ch < 1 || ch > static_cast<int>(_relay->getChannelCount())) {
                request->send(400, "application/json", "{\"error\":\"Invalid channel\"}");
                return;
            }

            const char* action = doc["action"] | "";
            bool ok = false;
            if (strcmp(action, "on") == 0) {
                ok = _relay->setState(static_cast<uint8_t>(ch), true);
            } else if (strcmp(action, "off") == 0) {
                ok = _relay->setState(static_cast<uint8_t>(ch), false);
            } else if (strcmp(action, "toggle") == 0) {
                ok = _relay->toggle(static_cast<uint8_t>(ch));
            } else {
                request->send(400, "application/json", "{\"error\":\"Invalid action\"}");
                return;
            }

            if (!ok) {
                request->send(400, "application/json", "{\"error\":\"Invalid channel\"}");
                return;
            }

            // Broadcast updated state to all WebSocket clients
            _ws->broadcastState();

            // Build response: {id, state}
            JsonDocument resp_doc;
            resp_doc["id"]    = ch;
            resp_doc["state"] = _relay->getState(static_cast<uint8_t>(ch));
            String body;
            body.reserve(32);
            serializeJson(resp_doc, body);

            AsyncWebServerResponse* resp =
                request->beginResponse(200, "application/json", body);
            addCorsHeaders(resp);
            request->send(resp);
        },
        nullptr,
        [](AsyncWebServerRequest* request,
           uint8_t* data, size_t len,
           size_t index, size_t total)
        {
            if (index == 0) {
                request->_tempObject = new String();
                reinterpret_cast<String*>(request->_tempObject)->reserve(total);
            }
            reinterpret_cast<String*>(request->_tempObject)
                ->concat(reinterpret_cast<const char*>(data), len);
        }
    );

    // ── GET /api/config/relays — return relay channel configuration ───────────
    _server->on("/api/config/relays", HTTP_GET,
        [this](AsyncWebServerRequest* request) {
            const JsonDocument& rDoc = _config->relayConfig();

            JsonDocument out;
            out["channelCount"] = rDoc["channelCount"] | 2;

            JsonArray outChannels = out["channels"].to<JsonArray>();
            JsonArrayConst srcChannels = rDoc["channels"].as<JsonArrayConst>();
            for (JsonObjectConst ch : srcChannels) {
                JsonObject o = outChannels.add<JsonObject>();
                o["id"]             = ch["id"]             | 0;
                o["pin"]            = ch["pin"]             | 0;
                o["name"]           = ch["name"]            | "";
                o["powerOnState"]   = ch["powerOnState"]    | "last";
                o["pulseDuration"]  = ch["pulseDuration"]   | 0;
                o["interlockGroup"] = ch["interlockGroup"]  | 0;
            }

            String body;
            body.reserve(512);
            serializeJson(out, body);

            AsyncWebServerResponse* resp =
                request->beginResponse(200, "application/json", body);
            addCorsHeaders(resp);
            request->send(resp);
        }
    );

    // ── POST /api/config/relays — update relay channel configuration ──────────
    // Body is accumulated via the body handler into _tempObject (String*),
    // parsed and freed in the request handler — identical to /api/config/wifi.
    _server->on("/api/config/relays", HTTP_POST,
        [this](AsyncWebServerRequest* request) {
            String* bodyStr = reinterpret_cast<String*>(request->_tempObject);
            if (!bodyStr) {
                AsyncWebServerResponse* resp =
                    request->beginResponse(400, "application/json",
                                           "{\"error\":\"No body\"}");
                addCorsHeaders(resp);
                request->send(resp);
                return;
            }
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, *bodyStr);
            delete bodyStr;
            request->_tempObject = nullptr;

            if (err) {
                AsyncWebServerResponse* resp =
                    request->beginResponse(400, "application/json",
                                           "{\"error\":\"Invalid JSON\"}");
                addCorsHeaders(resp);
                request->send(resp);
                return;
            }

            // Validate channel count: must be 1–MAX_CHANNELS (4).
            int channelCount = doc["channelCount"] | -1;
            if (channelCount < 1 || channelCount > MAX_CHANNELS) {
                AsyncWebServerResponse* resp =
                    request->beginResponse(400, "application/json",
                                           "{\"error\":\"Invalid channel count\"}");
                addCorsHeaders(resp);
                request->send(resp);
                return;
            }

            // Boot-sensitive GPIO detection (warn but still save).
            static const uint8_t BOOT_PINS[] = {0, 2, 15};
            static const uint8_t BOOT_PINS_COUNT =
                sizeof(BOOT_PINS) / sizeof(BOOT_PINS[0]);

            bool         bootWarning = false;
            JsonDocument warnDoc;
            JsonArray    warnPins    = warnDoc["pins"].to<JsonArray>();

            JsonArrayConst inChannels = doc["channels"].as<JsonArrayConst>();
            for (JsonObjectConst ch : inChannels) {
                int pin = ch["pin"] | -1;
                if (pin < 0) continue;
                for (uint8_t i = 0; i < BOOT_PINS_COUNT; ++i) {
                    if (static_cast<uint8_t>(pin) == BOOT_PINS[i]) {
                        // Only add each pin once.
                        bool already = false;
                        for (int wp : warnPins) {
                            if (wp == pin) { already = true; break; }
                        }
                        if (!already) {
                            warnPins.add(pin);
                            bootWarning = true;
                        }
                        break;
                    }
                }
            }

            // Write validated channel data into the mutable relay config doc.
            JsonDocument& rDoc = _config->relayConfigMut();
            rDoc["channelCount"] = channelCount;

            JsonArray storedChannels = rDoc["channels"].to<JsonArray>();
            int idx = 0;
            for (JsonObjectConst ch : inChannels) {
                if (idx >= channelCount) break;

                // Clamp channel name to 20 UTF-8 characters (String-level).
                // ArduinoJson represents UTF-8 strings natively, so substring()
                // operates on byte offsets — safe here because the UI sends
                // well-formed UTF-8 and the 20-char limit matches data-model.md.
                String nameStr = ch["name"] | "";
                if (nameStr.length() > 20) {
                    nameStr = nameStr.substring(0, 20);
                }

                // Validate powerOnState; fall back to "last" if unrecognised.
                const char* pos = ch["powerOnState"] | "last";
                if (strcmp(pos, "on") != 0 && strcmp(pos, "off") != 0) {
                    pos = "last";
                }

                JsonObject o = storedChannels.add<JsonObject>();
                o["id"]             = ch["id"]            | (idx + 1);
                o["pin"]            = ch["pin"]            | 0;
                o["name"]           = nameStr;
                o["powerOnState"]   = pos;
                o["pulseDuration"]  = ch["pulseDuration"]  | 0;
                o["interlockGroup"] = ch["interlockGroup"] | 0;

                ++idx;
            }

            // Persist to LittleFS, re-init relay GPIOs, notify WebSocket clients.
            _config->saveRelays();
            _relay->begin(*_config);
            _ws->broadcastConfigChanged("relays");
            _ws->broadcastState();  // refresh relay names on dashboard

            // Build success response.
            JsonDocument respDoc;
            respDoc["success"]              = true;
            respDoc["bootSensitiveWarning"] = bootWarning;
            if (bootWarning) {
                JsonArray rPins = respDoc["warningPins"].to<JsonArray>();
                for (int p : warnPins) {
                    rPins.add(p);
                }
            }

            String body;
            body.reserve(128);
            serializeJson(respDoc, body);

            AsyncWebServerResponse* resp =
                request->beginResponse(200, "application/json", body);
            addCorsHeaders(resp);
            request->send(resp);
        },
        nullptr,
        [](AsyncWebServerRequest* request,
           uint8_t* data, size_t len,
           size_t index, size_t total)
        {
            if (index == 0) {
                request->_tempObject = new String();
                reinterpret_cast<String*>(request->_tempObject)->reserve(total);
            }
            reinterpret_cast<String*>(request->_tempObject)
                ->concat(reinterpret_cast<const char*>(data), len);
        }
    );

    // ── GET /api/config/mqtt — return MQTT settings (password omitted) ───────
    _server->on("/api/config/mqtt", HTTP_GET,
        [this](AsyncWebServerRequest* request) {
            const JsonDocument& mDoc = _config->mqttConfig();
            JsonDocument out;
            out["enabled"]  = mDoc["enabled"]  | false;
            out["broker"]   = mDoc["broker"]   | "broker.emqx.io";
            out["port"]     = mDoc["port"]     | 1883;
            out["prefix"]   = mDoc["prefix"]   | "elmahdy";
            out["connected"] = _mqttManager ? _mqttManager->isConnected() : false;

            String body;
            body.reserve(256);
            serializeJson(out, body);

            AsyncWebServerResponse* resp =
                request->beginResponse(200, "application/json", body);
            addCorsHeaders(resp);
            request->send(resp);
        }
    );

    // ── POST /api/config/mqtt — save MQTT settings ────────────────────────────
    // Body is accumulated in _tempObject (String*) by the body handler, then
    // parsed and freed in the request handler — identical pattern to
    // /api/config/wifi and /api/config/relays.
    _server->on("/api/config/mqtt", HTTP_POST,
        [this](AsyncWebServerRequest* request) {
            String* bodyStr = reinterpret_cast<String*>(request->_tempObject);
            if (!bodyStr) {
                AsyncWebServerResponse* resp =
                    request->beginResponse(400, "application/json",
                                           "{\"error\":\"No body\"}");
                addCorsHeaders(resp);
                request->send(resp);
                return;
            }
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, *bodyStr);
            delete bodyStr;
            request->_tempObject = nullptr;

            if (err) {
                AsyncWebServerResponse* resp =
                    request->beginResponse(400, "application/json",
                                           "{\"error\":\"Invalid JSON\"}");
                addCorsHeaders(resp);
                request->send(resp);
                return;
            }

            // Validate broker: required, max 64 chars
            const char* broker = doc["broker"] | "";
            if (strlen(broker) == 0 || strlen(broker) > 64) {
                AsyncWebServerResponse* resp =
                    request->beginResponse(400, "application/json",
                                           "{\"error\":\"broker required, max 64 chars\"}");
                addCorsHeaders(resp);
                request->send(resp);
                return;
            }

            // Validate port: 1-65535
            int port = doc["port"] | -1;
            if (port < 1 || port > 65535) {
                AsyncWebServerResponse* resp =
                    request->beginResponse(400, "application/json",
                                           "{\"error\":\"port must be 1-65535\"}");
                addCorsHeaders(resp);
                request->send(resp);
                return;
            }

            // Validate prefix: max 20 chars
            const char* prefix = doc["prefix"] | "elmahdy";
            if (strlen(prefix) > 20) {
                AsyncWebServerResponse* resp =
                    request->beginResponse(400, "application/json",
                                           "{\"error\":\"prefix max 20 chars\"}");
                addCorsHeaders(resp);
                request->send(resp);
                return;
            }

            // Write validated values into the mutable MQTT config doc
            JsonDocument& mDoc = _config->mqttConfigMut();
            mDoc["enabled"]  = doc["enabled"].is<bool>() ? doc["enabled"].as<bool>() : true;
            mDoc["broker"]   = broker;
            mDoc["port"]     = static_cast<uint16_t>(port);
            mDoc["username"] = "";
            mDoc["password"] = "";
            mDoc["prefix"]   = prefix;

            _config->saveMqtt();

            // Broadcast before we reboot
            _ws->broadcastConfigChanged("mqtt");

            AsyncWebServerResponse* resp =
                request->beginResponse(200, "application/json",
                                       "{\"success\":true,\"restarting\":true}");
            addCorsHeaders(resp);
            request->send(resp);

            // Schedule reboot
            _restartTicker.once_ms(1200, []() { ESP.restart(); });
        },
        nullptr,
        [](AsyncWebServerRequest* request,
           uint8_t* data, size_t len,
           size_t index, size_t total)
        {
            if (index == 0) {
                request->_tempObject = new String();
                reinterpret_cast<String*>(request->_tempObject)->reserve(total);
            }
            reinterpret_cast<String*>(request->_tempObject)
                ->concat(reinterpret_cast<const char*>(data), len);
        }
    );

    // ── GET /api/lang/{code} — serve language pack JSON from LittleFS ────────
    // T025: code must be "ar" or "en"; file lives at /lang_{code}.json in root.
    _server->on("/api/lang/*", HTTP_GET,
        [](AsyncWebServerRequest* request) {
            const String& url = request->url();
            String code = url.substring(url.lastIndexOf('/') + 1);
            code.toLowerCase();
            if (code != "ar" && code != "en") {
                request->send(400, "application/json",
                              "{\"error\":\"Invalid language code\"}");
                return;
            }
            String path = "/lang_" + code + ".json";
            if (!LittleFS.exists(path)) {
                request->send(404, "application/json",
                              "{\"error\":\"Language pack not found\"}");
                return;
            }
            AsyncWebServerResponse* resp =
                request->beginResponse(LittleFS, path, "application/json");
            addCorsHeaders(resp);
            request->send(resp);
        }
    );

    // ── GET /api/timers — list all active timers ──────────────────────────────
    // Response: {"timers":[{…}],"count":N,"max":8}
    _server->on("/api/timers", HTTP_GET,
        [this](AsyncWebServerRequest* request) {
            JsonDocument out;
            uint8_t count = 0;

            if (_timerEngine) {
                count = _timerEngine->getTimers(out);
            } else {
                out["timers"].to<JsonArray>();
            }

            out["count"] = count;
            out["max"]   = MAX_TIMERS;

            String body;
            body.reserve(512);
            serializeJson(out, body);

            AsyncWebServerResponse* resp =
                request->beginResponse(200, "application/json", body);
            addCorsHeaders(resp);
            request->send(resp);
        }
    );

    // ── POST /api/timer — create a new countdown timer ────────────────────────
    // Request:  {"channel":1,"type":"countdown","targetState":"off","duration":1800000}
    // Response: {"id":3,"success":true}
    // Error:    {"error":"Timer limit reached (max 8)"} 400
    //           {"error":"Invalid JSON"}               400
    //           {"error":"Missing required field"}     400
    _server->on("/api/timer", HTTP_POST,
        [this](AsyncWebServerRequest* request) {
            String* bodyStr = reinterpret_cast<String*>(request->_tempObject);
            if (!bodyStr) {
                AsyncWebServerResponse* resp =
                    request->beginResponse(400, "application/json",
                                           "{\"error\":\"No body\"}");
                addCorsHeaders(resp);
                request->send(resp);
                return;
            }

            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, *bodyStr);
            delete bodyStr;
            request->_tempObject = nullptr;

            if (err) {
                AsyncWebServerResponse* resp =
                    request->beginResponse(400, "application/json",
                                           "{\"error\":\"Invalid JSON\"}");
                addCorsHeaders(resp);
                request->send(resp);
                return;
            }

            const char* type        = doc["type"] | "countdown";
            const bool  isScheduled = (strcmp(type, "scheduled") == 0);

            // Validate channel — required for both types.
            if (!doc["channel"].is<uint8_t>()) {
                AsyncWebServerResponse* resp =
                    request->beginResponse(400, "application/json",
                                           "{\"error\":\"Missing required field: channel\"}");
                addCorsHeaders(resp);
                request->send(resp);
                return;
            }

            // Validate type-specific required fields.
            if (!isScheduled && !doc["duration"].is<uint32_t>()) {
                AsyncWebServerResponse* resp =
                    request->beginResponse(400, "application/json",
                                           "{\"error\":\"Missing required field: duration\"}");
                addCorsHeaders(resp);
                request->send(resp);
                return;
            }

            if (isScheduled &&
                (!doc["hour"].is<uint8_t>() || !doc["minute"].is<uint8_t>()))
            {
                AsyncWebServerResponse* resp =
                    request->beginResponse(400, "application/json",
                                           "{\"error\":\"Missing required field: hour/minute\"}");
                addCorsHeaders(resp);
                request->send(resp);
                return;
            }

            if (!_timerEngine) {
                AsyncWebServerResponse* resp =
                    request->beginResponse(503, "application/json",
                                           "{\"error\":\"Timer engine not ready\"}");
                addCorsHeaders(resp);
                request->send(resp);
                return;
            }

            const uint8_t  channel     = doc["channel"].as<uint8_t>();
            const char*    targetState = doc["targetState"] | "off";

            uint16_t id = 0;
            if (isScheduled) {
                const uint8_t  hour       = doc["hour"].as<uint8_t>();
                const uint8_t  minute     = doc["minute"].as<uint8_t>();
                const char*    repeatMode = doc["repeatMode"] | "daily";
                const uint8_t  dayMask    = doc["dayMask"]    | 0;
                id = _timerEngine->createScheduled(channel, targetState,
                                                   hour, minute,
                                                   repeatMode, dayMask);
            } else {
                const uint32_t duration = doc["duration"].as<uint32_t>();
                id = _timerEngine->createCountdown(channel, duration, targetState);
            }

            if (id == 0) {
                char errBuf[64];
                snprintf(errBuf, sizeof(errBuf),
                         "{\"error\":\"Timer limit reached (max %u)\"}", MAX_TIMERS);
                AsyncWebServerResponse* resp =
                    request->beginResponse(400, "application/json", errBuf);
                addCorsHeaders(resp);
                request->send(resp);
                return;
            }

            JsonDocument respDoc;
            respDoc["id"]      = id;
            respDoc["success"] = true;

            String body;
            body.reserve(32);
            serializeJson(respDoc, body);

            AsyncWebServerResponse* resp =
                request->beginResponse(200, "application/json", body);
            addCorsHeaders(resp);
            request->send(resp);
        },
        nullptr,
        [](AsyncWebServerRequest* request,
           uint8_t* data, size_t len,
           size_t index, size_t total)
        {
            if (index == 0) {
                request->_tempObject = new String();
                reinterpret_cast<String*>(request->_tempObject)->reserve(total);
            }
            reinterpret_cast<String*>(request->_tempObject)
                ->concat(reinterpret_cast<const char*>(data), len);
        }
    );

    // ── DELETE /api/timer/{id} — cancel a timer by ID ────────────────────────
    // Response: {"success":true}
    // Error:    {"error":"Timer not found"} 404
    _server->on("/api/timer/*", HTTP_DELETE,
        [this](AsyncWebServerRequest* request) {
            const String& url = request->url();
            uint16_t timerId = static_cast<uint16_t>(url.substring(url.lastIndexOf('/') + 1).toInt());

            if (!_timerEngine) {
                AsyncWebServerResponse* resp =
                    request->beginResponse(503, "application/json",
                                           "{\"error\":\"Timer engine not ready\"}");
                addCorsHeaders(resp);
                request->send(resp);
                return;
            }

            if (!_timerEngine->cancel(timerId)) {
                AsyncWebServerResponse* resp =
                    request->beginResponse(404, "application/json",
                                           "{\"error\":\"Timer not found\"}");
                addCorsHeaders(resp);
                request->send(resp);
                return;
            }

            AsyncWebServerResponse* resp =
                request->beginResponse(200, "application/json",
                                       "{\"success\":true}");
            addCorsHeaders(resp);
            request->send(resp);
        }
    );

    // ── GET /api/config/system — read system configuration ───────────────────
    _server->on("/api/config/system", HTTP_GET,
        [this](AsyncWebServerRequest* request) {
            const JsonDocument& sys = _config->systemConfig();
            String body;
            serializeJson(sys, body);
            AsyncWebServerResponse* resp =
                request->beginResponse(200, "application/json", body);
            addCorsHeaders(resp);
            request->send(resp);
        }
    );

    // ── POST /api/config/system — save system configuration ──────────────────
    _server->on("/api/config/system", HTTP_POST,
        [this](AsyncWebServerRequest* request) {
            String* bodyStr = request->_tempObject
                ? reinterpret_cast<String*>(request->_tempObject) : nullptr;
            if (!bodyStr) {
                request->send(400, "application/json", "{\"error\":\"No body\"}");
                return;
            }
            JsonDocument doc;
            if (deserializeJson(doc, *bodyStr)) {
                delete bodyStr; request->_tempObject = nullptr;
                request->send(400, "application/json", "{\"error\":\"Bad JSON\"}");
                return;
            }
            delete bodyStr; request->_tempObject = nullptr;

            JsonDocument& sys = _config->systemConfigMut();
            if (doc["buzzerEnabled"].is<bool>())  sys["buzzerEnabled"]  = doc["buzzerEnabled"].as<bool>();
            if (doc["ledEnabled"].is<bool>())      sys["ledEnabled"]     = doc["ledEnabled"].as<bool>();
            if (doc["resetEnabled"].is<bool>())    sys["resetEnabled"]   = doc["resetEnabled"].as<bool>();
            if (doc["buzzerPin"].is<int>())        sys["buzzerPin"]      = doc["buzzerPin"].as<int>();
            if (doc["resetPin"].is<int>())         sys["resetPin"]       = doc["resetPin"].as<int>();
            if (doc["hostname"].is<const char*>()) {
                sys["hostname"] = sanitizeHostname(doc["hostname"].as<const char*>());
            }
            if (doc["mdnsEnabled"].is<bool>())     sys["mdnsEnabled"]    = doc["mdnsEnabled"].as<bool>();
            if (doc["timezoneOffset"].is<int>())   sys["timezoneOffset"] = doc["timezoneOffset"].as<int>();
            if (doc["language"].is<const char*>()) sys["language"]       = doc["language"].as<const char*>();
            _config->saveSystem();
            if (doc["buzzerEnabled"].is<bool>())
                buzzerController.setEnabled(doc["buzzerEnabled"].as<bool>());
            if (_wifi && (doc["hostname"].is<const char*>() || doc["mdnsEnabled"].is<bool>())) {
                _wifi->refreshMdns();
            }

            if (_ws) { _ws->broadcastConfigChanged("system"); }
            AsyncWebServerResponse* resp =
                request->beginResponse(200, "application/json",
                                       "{\"success\":true,\"restarting\":true}");
            addCorsHeaders(resp);
            request->send(resp);

            // Deferred restart — 1200 ms lets the TCP ACK flush before reset.
            _restartTicker.once_ms(1200, []() { ESP.restart(); });
        },
        nullptr,
        [](AsyncWebServerRequest* request,
           uint8_t* data, size_t len, size_t index, size_t /*total*/)
        {
            if (index == 0) { request->_tempObject = new String(); }
            reinterpret_cast<String*>(request->_tempObject)
                ->concat(reinterpret_cast<const char*>(data), len);
                
        }
        
    );

    // ── GET /api/scenes — list all scenes ────────────────────────────────────
    _server->on("/api/scenes", HTTP_GET,
        [this](AsyncWebServerRequest* request) {
            JsonDocument doc;
            if (_sceneManager) {
                _sceneManager->getScenes(doc);
            } else {
                doc["scenes"].to<JsonArray>();
            }
            String body; serializeJson(doc, body);
            AsyncWebServerResponse* resp =
                request->beginResponse(200, "application/json", body);
            addCorsHeaders(resp);
            request->send(resp);
        }
    );

    // ── POST /api/scene/{name}/activate — trigger a scene ────────────────────
    // Registered BEFORE the exact /api/scene create handler because this
    // library's exact-URI matching also matches sub-paths (/api/scene/*),
    // so the wildcard must win first for activate requests.
    _server->on("/api/scene/*", HTTP_POST,
        [this](AsyncWebServerRequest* request) {
            const String& url = request->url();
            if (!url.endsWith("/activate")) {
                AsyncWebServerResponse* resp =
                    request->beginResponse(404, "application/json",
                                           "{\"error\":\"Not found\"}");
                addCorsHeaders(resp);
                request->send(resp);
                return;
            }
            // Extract name: /api/scene/NAME/activate
            String inner = url.substring(strlen("/api/scene/"));
            String name  = urlDecode(inner.substring(0, inner.length() - strlen("/activate")));
            if (!_sceneManager || name.isEmpty()) {
                request->send(400, "application/json", "{\"error\":\"Missing name\"}");
                return;
            }
            if (_sceneManager->activate(name.c_str())) {
                if (_ws) { _ws->broadcastState(); }
                AsyncWebServerResponse* resp =
                    request->beginResponse(200, "application/json",
                                           "{\"success\":true}");
                addCorsHeaders(resp); request->send(resp);
            } else {
                AsyncWebServerResponse* resp =
                    request->beginResponse(404, "application/json",
                                           "{\"error\":\"Scene not found\"}");
                addCorsHeaders(resp); request->send(resp);
            }
        }
    );

    // ── POST /api/scene — create a scene ─────────────────────────────────────
    _server->on("/api/scene", HTTP_POST,
        [this](AsyncWebServerRequest* request) {
            String* bodyStr = request->_tempObject
                ? reinterpret_cast<String*>(request->_tempObject) : nullptr;
            if (!bodyStr) {
                request->send(400, "application/json", "{\"error\":\"No body\"}");
                return;
            }
            JsonDocument doc;
            if (deserializeJson(doc, *bodyStr)) {
                delete bodyStr; request->_tempObject = nullptr;
                request->send(400, "application/json", "{\"error\":\"Bad JSON\"}");
                return;
            }
            delete bodyStr; request->_tempObject = nullptr;

            const char* name = doc["name"] | "";
            if (!_sceneManager || name[0] == '\0') {
                AsyncWebServerResponse* resp =
                    request->beginResponse(400, "application/json",
                                           "{\"error\":\"Missing name\"}");
                addCorsHeaders(resp); request->send(resp); return;
            }
            JsonObject states = doc["states"].as<JsonObject>();
            JsonObjectConst schedObj = doc["schedule"].is<JsonObjectConst>()
                ? doc["schedule"].as<JsonObjectConst>()
                : JsonObjectConst();
            if (_sceneManager->createScene(name, states, schedObj)) {
                if (_ws) { _ws->broadcastConfigChanged("scenes"); }
                AsyncWebServerResponse* resp =
                    request->beginResponse(200, "application/json",
                                           "{\"success\":true}");
                addCorsHeaders(resp); request->send(resp);
            } else {
                AsyncWebServerResponse* resp =
                    request->beginResponse(409, "application/json",
                                           "{\"error\":\"Scene exists or limit reached\"}");
                addCorsHeaders(resp); request->send(resp);
            }
        },
        nullptr,
        [](AsyncWebServerRequest* request,
           uint8_t* data, size_t len, size_t index, size_t /*total*/)
        {
            if (index == 0) { request->_tempObject = new String(); }
            reinterpret_cast<String*>(request->_tempObject)
                ->concat(reinterpret_cast<const char*>(data), len);
        }
    );

    // ── DELETE /api/scene/{name} — delete a scene ────────────────────────────
    _server->on("/api/scene/*", HTTP_DELETE,
        [this](AsyncWebServerRequest* request) {
            const String& url = request->url();
            String name = urlDecode(url.substring(strlen("/api/scene/")));
            if (!_sceneManager || name.isEmpty()) {
                request->send(400, "application/json", "{\"error\":\"Missing name\"}");
                return;
            }
            if (_sceneManager->deleteScene(name.c_str())) {
                if (_ws) { _ws->broadcastConfigChanged("scenes"); }
                AsyncWebServerResponse* resp =
                    request->beginResponse(200, "application/json",
                                           "{\"success\":true}");
                addCorsHeaders(resp); request->send(resp);
            } else {
                AsyncWebServerResponse* resp =
                    request->beginResponse(404, "application/json",
                                           "{\"error\":\"Scene not found\"}");
                addCorsHeaders(resp); request->send(resp);
            }
        }
    );

    // ── POST /api/system/buzzer-test — fire one short beep ───────────────────
    _server->on("/api/system/buzzer-test", HTTP_POST,
        [](AsyncWebServerRequest* request) {
            buzzerController.beepShort();
            AsyncWebServerResponse* resp =
                request->beginResponse(200, "application/json", "{\"success\":true}");
            resp->addHeader("Access-Control-Allow-Origin", "*");
            request->send(resp);
        }
    );

    // ── POST /api/system/reboot ───────────────────────────────────────────────
    _server->on("/api/system/reboot", HTTP_POST,
        [](AsyncWebServerRequest* request) {
            AsyncWebServerResponse* resp =
                request->beginResponse(200, "application/json",
                                       "{\"success\":true}");
            resp->addHeader("Access-Control-Allow-Origin", "*");
            request->send(resp);
            _restartTicker.once_ms(500, []() { ESP.restart(); });
        }
    );

    // ── POST /api/system/reset (factory reset) ────────────────────────────────
    _server->on("/api/system/reset", HTTP_POST,
        [this](AsyncWebServerRequest* request) {
            AsyncWebServerResponse* resp =
                request->beginResponse(200, "application/json",
                                       "{\"success\":true}");
            resp->addHeader("Access-Control-Allow-Origin", "*");
            request->send(resp);
            if (_config) { _config->resetAll(); }
            _restartTicker.once_ms(500, []() { ESP.restart(); });
        }
    );

    // ── GET /api/backup — download full config as JSON ───────────────────────
    _server->on("/api/backup", HTTP_GET,
        [this](AsyncWebServerRequest* request) {
            JsonDocument doc;
            // Shallow-copy each section into the backup document
            doc["wifi"].set(_config->wifiConfig());
            doc["mqtt"].set(_config->mqttConfig());
            doc["relays"].set(_config->relayConfig());
            doc["relayState"].set(_config->relayStateConfig());
            doc["timers"].set(_config->timerConfig());
            doc["scenes"].set(_config->sceneConfig());
            doc["system"].set(_config->systemConfig());
            String body; serializeJson(doc, body);
            AsyncWebServerResponse* resp =
                request->beginResponse(200, "application/json", body);
            resp->addHeader("Content-Disposition",
                            "attachment; filename=\"backup.json\"");
            addCorsHeaders(resp);
            request->send(resp);
        }
    );

    // ── POST /api/restore — restore config from backup JSON ──────────────────
    _server->on("/api/restore", HTTP_POST,
        [this](AsyncWebServerRequest* request) {
            String* bodyStr = request->_tempObject
                ? reinterpret_cast<String*>(request->_tempObject) : nullptr;
            if (!bodyStr) {
                request->send(400, "application/json", "{\"error\":\"No body\"}");
                return;
            }
            JsonDocument doc;
            if (deserializeJson(doc, *bodyStr)) {
                delete bodyStr; request->_tempObject = nullptr;
                request->send(400, "application/json", "{\"error\":\"Bad JSON\"}");
                return;
            }
            delete bodyStr; request->_tempObject = nullptr;

            bool ok = true;
            if (doc["wifi"].is<JsonObject>()) {
                _config->wifiConfigMut().set(doc["wifi"]); ok &= _config->saveWifi(); }
            if (doc["mqtt"].is<JsonObject>()) {
                _config->mqttConfigMut().set(doc["mqtt"]); ok &= _config->saveMqtt(); }
            if (doc["relays"].is<JsonObject>()) {
                _config->relayConfigMut().set(doc["relays"]); ok &= _config->saveRelays(); }
            if (doc["relayState"].is<JsonObject>()) {
                _config->relayStateConfigMut().set(doc["relayState"]); ok &= _config->saveRelayState(); }
            if (doc["timers"].is<JsonObject>()) {
                _config->timerConfigMut().set(doc["timers"]); ok &= _config->saveTimers(); }
            if (doc["scenes"].is<JsonObject>()) {
                _config->sceneConfigMut().set(doc["scenes"]); ok &= _config->saveScenes(); }
            if (doc["system"].is<JsonObject>()) {
                _config->systemConfigMut().set(doc["system"]); ok &= _config->saveSystem(); }

            AsyncWebServerResponse* resp =
                request->beginResponse(ok ? 200 : 500, "application/json",
                                       ok ? "{\"success\":true}" : "{\"error\":\"Partial restore\"}");
            addCorsHeaders(resp);
            request->send(resp);
            if (ok) { _restartTicker.once_ms(1000, []() { ESP.restart(); }); }
        },
        nullptr,
        [](AsyncWebServerRequest* request,
           uint8_t* data, size_t len, size_t index, size_t total)
        {
            if (index == 0) {
                request->_tempObject = new String();
                reinterpret_cast<String*>(request->_tempObject)->reserve(total);
            }
            reinterpret_cast<String*>(request->_tempObject)
                ->concat(reinterpret_cast<const char*>(data), len);
        }
    );

    // ── POST /api/system/update — OTA firmware upload ─────────────────────────
    _server->on("/api/system/update", HTTP_POST,
        [](AsyncWebServerRequest* request) {
            AsyncWebServerResponse* resp = request->beginResponse(
                _otaError ? 500 : 200,
                "application/json",
                _otaError ? "{\"error\":\"Update failed\"}" : "{\"success\":true}"
            );
            resp->addHeader("Access-Control-Allow-Origin", "*");
            request->send(resp);
            if (!_otaError) {
                _restartTicker.once_ms(1000, []() { ESP.restart(); });
            }
        },
        [](AsyncWebServerRequest* request, String filename,
           size_t index, uint8_t* data, size_t len, bool final) {
            if (index == 0) {
                Serial.printf_P(PSTR("[OTA] Start: %s\n"), filename.c_str());
                _otaError = false;
                if (!Update.begin(ESP.getFreeSketchSpace())) {
                    _otaError = true;
                    Update.printError(Serial);
                }
            }
            if (!_otaError && len) {
                if (Update.write(data, len) != len) {
                    _otaError = true;
                }
            }
            if (final) {
                if (!_otaError && Update.end(true)) {
                    Serial.printf_P(PSTR("[OTA] Success: %u bytes\n"),
                                    static_cast<unsigned>(index + len));
                } else if (_otaError) {
                    Update.printError(Serial);
                }
            }
        }
    );

    // ── Static file handler — LittleFS /www/ with gzip auto-detection ────────
    _server->serveStatic("/", LittleFS, "/www/")
            .setDefaultFile("index.html");

    // ── SPA fallback — any unmatched GET serves index.html (or .gz) ──────────
    _server->onNotFound(
        [](AsyncWebServerRequest* request) {
            if (request->method() == HTTP_GET) {
                const char* mime = "text/html; charset=utf-8";
                if (!serveFileWithGzip(request, "/www/index.html", mime)) {
                    request->send(404, "text/plain", "Not Found");
                }
            } else {
                AsyncWebServerResponse* resp =
                    request->beginResponse(404, "application/json",
                                           "{\"error\":\"Not found\"}");
                addCorsHeaders(resp);
                request->send(resp);
            }
        }
    );
}
