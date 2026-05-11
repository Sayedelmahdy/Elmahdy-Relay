/*
 * web_server.h — ESPAsyncWebServer wrapper: static file serving, REST API
 *                routes, CORS, SPA fallback, and WebSocket endpoint attachment.
 *
 * Target : ESP8266 (ESP-12F / NodeMCU), Arduino Core 3.x, C++17
 * Build  : PlatformIO (espressif8266 + ESPAsyncWebServer-esphome ^3.3)
 *
 * Design notes
 * ────────────
 * • LittleFS.begin() is NOT called here — ConfigManager::begin() mounts it
 *   before WebServer::begin() is invoked.
 * • All routes are registered once in registerRoutes(); later tasks (T012,
 *   T016, …) add their own routes directly to server() before begin() is
 *   called on the AsyncWebServer.
 * • CORS headers (Access-Control-Allow-Origin: *) are applied to every
 *   /api/ response via addCorsHeaders().
 * • Static files are served from LittleFS /www/.  Gzip-compressed variants
 *   (.gz suffix) are detected and served with Content-Encoding: gzip so the
 *   browser can decompress transparently.
 * • The SPA fallback handler catches all unmatched GET requests and serves
 *   /www/index.html (or its .gz variant).
 * • No delay() calls — entirely non-blocking.
 */

#ifndef WEB_SERVER_H_
#define WEB_SERVER_H_

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>

// Forward declarations — full headers are included in web_server.cpp to
// avoid pulling transitively heavy includes into every translation unit that
// includes this header.
class ConfigManager;
class RelayController;
class WebSocketHandler;
class SceneManager;
class WiFiManager;
class TimerEngine;
class MqttManager;

/* ─────────────────────────────────────────────────────────────────────────────
 * MIME type table
 * The entries cover every file extension expected under /www/.
 * ───────────────────────────────────────────────────────────────────────────*/
struct MimeEntry {
    const char* ext;   // Extension without leading dot, lowercase
    const char* mime;  // MIME type string
};

static constexpr MimeEntry MIME_TABLE[] = {
    { "html", "text/html; charset=utf-8"      },
    { "css",  "text/css"                      },
    { "js",   "application/javascript"        },
    { "json", "application/json"              },
    { "png",  "image/png"                     },
    { "ico",  "image/x-icon"                  },
    { "svg",  "image/svg+xml"                 },
    { "webmanifest", "application/manifest+json" },
};
static constexpr size_t MIME_TABLE_SIZE =
    sizeof(MIME_TABLE) / sizeof(MIME_TABLE[0]);

/* ─────────────────────────────────────────────────────────────────────────────
 * WebServer
 * ───────────────────────────────────────────────────────────────────────────*/
class WebServer {
public:
    WebServer();
    ~WebServer() = default;

    /*
     * begin() — wire everything together and start listening on port 80.
     *
     * Must be called after:
     *   - LittleFS is mounted (done by ConfigManager::begin())
     *   - WiFi is up (STA or AP mode)
     *
     * Execution order:
     *   1. Allocate AsyncWebServer on port 80.
     *   2. Attach the WebSocket endpoint via ws.begin(server(), relay, scene).
     *   3. Call registerRoutes() to register all foundational REST routes and
     *      the static file handler.
     *   4. Call server.begin().
     *
     * Parameters
     *   config — read-only access for /api/info channel count.
     *   relay  — referenced by route handlers registered in later tasks.
     *   ws     — the WebSocket handler; its /ws endpoint is attached here.
     *   scene  — the scene manager; passed through to ws.begin().
     */
    void begin(ConfigManager&      config,
               RelayController&   relay,
               WebSocketHandler&  ws,
               SceneManager&      scene);

    /*
     * registerRoutes() — register all foundational HTTP routes.
     *
     * Routes registered here:
     *   GET  /api/info        — device metadata
     *   GET  /api/wifi/scan   — placeholder (returns {"networks":[]})
     *   OPTIONS *             — CORS preflight for all /api/ paths (→ 200)
     *   Static file handler   — LittleFS /www/ with gzip auto-detection
     *   404 / SPA fallback    — /www/index.html (or .gz variant)
     *
     * Called automatically by begin(); may also be called independently
     * before begin() if routes need to be added in a specific order.
     */
    void registerRoutes();

    /*
     * setWifiManager() — wire in a WiFiManager after begin() so WiFi REST
     * endpoints (/api/wifi/scan, /api/config/wifi) become functional.
     * Must be called before the first HTTP request is processed.
     */
    void setWifiManager(WiFiManager& wifi);

    /*
     * setTimerEngine() — wire in the TimerEngine so that timer REST
     * endpoints (/api/timers, /api/timer) become functional.
     * Must be called before the first HTTP request is processed.
     */
    void setTimerEngine(TimerEngine& timer);

    /*
     * setSceneManager() — wire in the SceneManager so that scene REST
     * endpoints become functional. Must be called before the first
     * HTTP request is processed.
     */
    void setSceneManager(SceneManager& scene);

    void setMqttManager(MqttManager& mqtt);

    /*
     * server() — expose the underlying AsyncWebServer so that other modules
     * can attach their own routes (OtaHandler, relay API, config API, …)
     * before WebServer::begin() is called, or to obtain a reference for
     * WebSocket attachment.
     *
     * The returned reference is valid for the lifetime of this WebServer
     * object (which is typically static / global).
     */
    AsyncWebServer& server();

private:
    /*
     * addCorsHeaders() — add the mandatory CORS headers to any response
     * destined for an /api/ endpoint.
     *
     *   Access-Control-Allow-Origin:  *
     *   Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS
     *   Access-Control-Allow-Headers: Content-Type, Authorization
     */
    static void addCorsHeaders(AsyncWebServerResponse* response);

    /*
     * mimeForPath() — resolve the MIME type string for the given file path
     * by examining the last dot-separated extension (ignoring .gz suffix).
     * Returns "application/octet-stream" for unknown extensions.
     */
    static const char* mimeForPath(const String& path);

    /*
     * serveFileWithGzip() — attempt to serve path from LittleFS.
     * Tries path + ".gz" first; if found, adds Content-Encoding: gzip and
     * sends it.  Falls back to plain path.  Returns false if neither exists.
     */
    static bool serveFileWithGzip(AsyncWebServerRequest* request,
                                  const String&          fsPath,
                                  const char*            mimeType);

    // Non-owning pointers; set during begin() and valid until destruction.
    ConfigManager*    _config;
    RelayController*  _relay;
    WebSocketHandler* _ws;
    WiFiManager*      _wifi;         // set via setWifiManager() after begin()
    TimerEngine*      _timerEngine;  // set via setTimerEngine() after begin()
    SceneManager*     _sceneManager; // set via setSceneManager() after begin()
    MqttManager*      _mqttManager;  // set via setMqttManager() after begin()

    // The AsyncWebServer is heap-allocated in begin() to avoid consuming
    // ~2 KB of BSS before setup() runs.  The pointer is never null after
    // begin() completes.
    AsyncWebServer*   _server;
};

#endif /* WEB_SERVER_H_ */
