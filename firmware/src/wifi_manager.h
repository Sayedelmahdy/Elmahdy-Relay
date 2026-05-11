/*
 * wifi_manager.h — AP+STA dual-mode WiFi with captive portal and auto-reconnect
 *
 * Target: ESP8266 (ESP-12F / NodeMCU), Arduino Core 3.x, C++17
 * Build:  PlatformIO (espressif8266)
 *
 * Design contract:
 *   - AP is NEVER disabled. WIFI_AP_STA mode is the only permitted mode.
 *   - DNSServer redirects all DNS queries to AP_IP (captive portal).
 *   - STA connection is attempted with up to MAX_STA_RETRIES retries, each
 *     gated by a millis()-based timeout (no delay() calls anywhere).
 *   - On disconnect, auto-reconnect is retried up to MAX_STA_RETRIES times;
 *     after exhaustion the STA FSM rests in FAILED state and the AP remains
 *     the sole access path.
 *   - Credentials are persisted to ConfigManager only after a successful
 *     first connection (connectSTA path). Auto-reconnect uses credentials
 *     already in ConfigManager without re-saving.
 *   - WiFi scanning is fully asynchronous (WIFI_SCAN_ASYNC, non-blocking).
 *   - mDNS registration is stubbed; the real call will be wired in when the
 *     mDNS hostname is confirmed in the system config (T024/T027 area).
 */

#ifndef WIFI_MANAGER_H_
#define WIFI_MANAGER_H_

#include <Arduino.h>
#include <functional>

#include <DNSServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>

#include "config.h"
#include "config_manager.h"

/* -------------------------------------------------------------------------
 * Tuning constants
 * ------------------------------------------------------------------------- */

/** Timeout per individual STA connection attempt (ms). */
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000UL;

/** Maximum STA connection attempts before declaring FAILED. */
static const uint8_t MAX_STA_RETRIES = 3;

/** DNS port for the captive portal server. */
static const uint8_t DNS_PORT = 53;

/** AP subnet mask — /24 matches the single /24 block used for the AP. */
static const IPAddress AP_SUBNET(255, 255, 255, 0);

/* -------------------------------------------------------------------------
 * STA connection state machine
 * ------------------------------------------------------------------------- */
enum class WifiState : uint8_t {
    IDLE,        ///< No STA credentials configured; AP-only mode.
    CONNECTING,  ///< WiFi.begin() called; waiting for CONNECTED or timeout.
    CONNECTED,   ///< STA link up.
    FAILED,      ///< All retries exhausted; AP-only mode until reboot/reconfigure.
};

/* -------------------------------------------------------------------------
 * WiFiManager
 * ------------------------------------------------------------------------- */
class WiFiManager {
public:
    WiFiManager() = default;
    ~WiFiManager() = default;

    /* Lifecycle ------------------------------------------------------------- */

    /**
     * Initialise AP+STA mode, start DNSServer for captive portal, and — if
     * WiFi credentials are present in config — kick off the first STA
     * connection attempt.
     *
     * Must be called once from setup() after ConfigManager::begin().
     * Never blocks: the STA FSM is driven by tick().
     */
    void begin(ConfigManager& config);

    /**
     * Per-loop service function.  Must be called every iteration of loop().
     *
     * Responsibilities:
     *   1. dnsServer.processNextRequest() — must run every loop to avoid
     *      DNS response queue build-up under captive-portal load.
     *   2. mDNS::update() (once STA is connected).
     *   3. STA FSM tick: advance CONNECTING → CONNECTED/FAILED, drive
     *      auto-reconnect after unexpected disconnect.
     */
    void tick();

    /* STA connection -------------------------------------------------------- */

    /**
     * Initiate a new STA connection with the given credentials.
     *
     * Resets the retry counter and transitions to CONNECTING state.
     * Credentials are NOT saved until the connection succeeds — call this
     * from the POST /api/config/wifi handler (T012); tick() will save on
     * success and fire the onConnected callback.
     *
     * @param ssid      Network SSID (null-terminated).
     * @param password  Network password (null-terminated, may be empty).
     */
    void connectSTA(const char* ssid, const char* password);

    /* WiFi scan ------------------------------------------------------------- */

    /**
     * Trigger an asynchronous background WiFi scan.
     * Returns immediately.  Call getScanResults() to poll completion.
     */
    void scan();

    /**
     * Return a JSON array string of scan results when the async scan has
     * completed, or an empty array if the scan is still in progress or has
     * not been started.
     *
     * Format: [{"ssid":"...","rssi":-70,"secure":true}, ...]
     *
     * Results are sorted by descending RSSI (strongest first).
     * Open networks have "secure":false.
     */
    String getScanResults() const;

    /* Accessors ------------------------------------------------------------- */

    /** AP SSID in the form "ElmahdyRelay_XXXX". */
    const String& getApSsid() const { return _apSsid; }

    /** True when the STA interface holds a valid IP address. */
    bool isConnected() const { return _state == WifiState::CONNECTED; }

    /** Current STA IP address (INADDR_ANY when not connected). */
    IPAddress getLocalIp() const { return WiFi.localIP(); }

    /** Current STA RSSI in dBm.  Returns 0 when not connected. */
    int8_t getRssi() const {
        return isConnected() ? static_cast<int8_t>(WiFi.RSSI()) : 0;
    }

    /** Current FSM state — useful for exposing status over the web API. */
    WifiState getState() const { return _state; }

    /** True when mDNS is enabled in config and the responder is running. */
    bool isMdnsRunning() const { return _mdnsRunning; }

    /** True when mDNS is enabled in system config. */
    bool isMdnsEnabled() const;

    /** Current configured mDNS hostname without the ".local" suffix. */
    String getMdnsHostname() const;

    /** Current configured mDNS URL, or an empty string when disabled. */
    String getMdnsUrl() const;

    /** Re-apply mDNS config after hostname or enabled setting changes. */
    void refreshMdns();

    /* Callback -------------------------------------------------------------- */

    /**
     * Register a callback invoked once each time the STA interface transitions
     * to CONNECTED.  Fired from tick() context (main loop thread), never from
     * an ISR.
     *
     * @param cb  std::function<void()> — may be a lambda.
     */
    void setOnConnected(std::function<void()> cb) { _onConnected = cb; }

private:
    /* Configuration pointer (non-owning) ------------------------------------ */
    ConfigManager* _config = nullptr;

    /* AP identity ----------------------------------------------------------- */
    String _apSsid;        ///< "ElmahdyRelay_XXXX" built during begin().

    /* DNS server for captive portal ----------------------------------------- */
    DNSServer _dns;

    /* STA state machine ----------------------------------------------------- */
    WifiState _state        = WifiState::IDLE;
    uint8_t   _retryCount   = 0;
    uint32_t  _connectStart = 0;    ///< millis() snapshot when connecting began.

    /* Credentials held during an in-progress connectSTA() call -------------- */
    String _pendingSsid;
    String _pendingPassword;

    /* Reconnect flag: true once we have successfully connected at least once  */
    bool _hadConnection = false;

    /* mDNS runtime state ---------------------------------------------------- */
    bool   _mdnsRunning = false;
    String _mdnsHostname;

    /* Callback -------------------------------------------------------------- */
    std::function<void()> _onConnected;

    /* Internal helpers ------------------------------------------------------ */

    /** Build the AP SSID from the last 4 hex characters of the MAC address. */
    void _buildApSsid();

    /** Configure and bring up the SoftAP interface. */
    void _startAp();

    /**
     * Call WiFi.begin() with _pendingSsid/_pendingPassword, record the
     * connection start time, and transition to CONNECTING.
     * Used by both connectSTA() (first attempt) and the FSM retry path.
     */
    void _beginConnect();

    /**
     * Tick the CONNECTING state: check for success or timeout, advance retry
     * counter, transition to CONNECTED or FAILED as appropriate.
     */
    void _tickConnecting();

    /**
     * Tick the CONNECTED state: detect unexpected disconnect and initiate
     * auto-reconnect (up to MAX_STA_RETRIES) using the stored credentials.
     */
    void _tickConnected();

    /**
     * Save the currently-connected STA credentials to ConfigManager and
     * persist to LittleFS.  Only called on a fresh connectSTA() success to
     * avoid unnecessary flash writes during auto-reconnect.
     */
    void _saveCredentials();

    /**
     * Register mDNS hostname "elmahdy-relay.local".
     * Stubbed until the system-config hostname is wired in (T024).
     */
    void _registerMdns();
};

/* -------------------------------------------------------------------------
 * Global singleton — extern declaration; definition is in wifi_manager.cpp
 * ------------------------------------------------------------------------- */
extern WiFiManager wifiManager;

#endif /* WIFI_MANAGER_H_ */
