/*
 * wifi_manager.cpp — AP+STA dual-mode WiFi with captive portal and auto-reconnect
 *
 * Target: ESP8266 (ESP-12F / NodeMCU), Arduino Core 3.x, C++17
 * Build:  PlatformIO (espressif8266)
 *
 * Implementation notes:
 *
 *   MAC-derived SSID suffix
 *   ───────────────────────
 *   WiFi.macAddress() returns a colon-separated string, e.g. "A4:CF:12:AB:CD:EF".
 *   We extract the last 4 hex characters by stripping colons from the last
 *   two octets (positions [12..16] after removing colons). This guarantees
 *   uniqueness across devices sharing the same firmware image.
 *
 *   No delay() contract
 *   ────────────────────
 *   All waiting is done via millis()-delta comparisons inside tick().  The FSM
 *   transitions are edge-triggered; the loop() caller drives them every cycle.
 *
 *   AP persistence guarantee
 *   ────────────────────────
 *   WiFi.mode() is called ONCE with WIFI_AP_STA in begin().  It is never called
 *   again.  Even on STA FAILED, the AP remains operational.
 *
 *   DNS / captive portal
 *   ────────────────────
 *   DNSServer is configured to resolve '*' (wildcard) to AP_IP so that any
 *   HTTP probe a mobile OS uses (e.g., connectivitycheck.gstatic.com) lands on
 *   our ESPAsyncWebServer, which T012 will configure to redirect unknown paths
 *   to the setup UI.
 *
 *   Scan result sorting
 *   ────────────────────
 *   WiFi.scanComplete() returns the count of discovered networks; we sort them
 *   by RSSI descending so the closest networks appear first in the picker UI.
 *   Sorting uses a simple insertion sort — the expected N is ≤ 20 networks so
 *   O(N²) is acceptable and avoids std::sort dependency.
 */

#include "wifi_manager.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <time.h>

/* ─────────────────────────────────────────────────────────────────────────────
 * Global singleton definition
 * ───────────────────────────────────────────────────────────────────────────── */
WiFiManager wifiManager;

/* ─────────────────────────────────────────────────────────────────────────────
 * Public: begin()
 * ───────────────────────────────────────────────────────────────────────────── */
void WiFiManager::begin(ConfigManager& config) {
    _config = &config;

    // Build the unique AP SSID first (needed for log output).
    _buildApSsid();

    // Force AP+STA mode unconditionally — this is the only WiFi.mode() call
    // in the entire module to honour the AP-persistence contract.
    WiFi.mode(WIFI_AP_STA);

    // Bring up the SoftAP with captive-portal DNS.
    _startAp();

    Serial.print(F("[WiFi] AP started: "));
    Serial.println(_apSsid);
    Serial.print(F("[WiFi] AP IP: "));
    Serial.println(WiFi.softAPIP());

    // If credentials exist in persistent config, attempt STA immediately.
    const JsonDocument& wifiDoc = _config->wifiConfig();
    const char* savedSsid = wifiDoc["ssid"] | "";
    const char* savedPass = wifiDoc["password"] | "";

    if (savedSsid[0] != '\0') {
        Serial.print(F("[WiFi] Stored SSID found, connecting to: "));
        Serial.println(savedSsid);
        _pendingSsid     = String(savedSsid);
        _pendingPassword = String(savedPass);
        _beginConnect();
    } else {
        Serial.println(F("[WiFi] No stored credentials — AP-only mode"));
        _state = WifiState::IDLE;
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Public: tick()
 * ───────────────────────────────────────────────────────────────────────────── */
void WiFiManager::tick() {
    // Step 1: Service DNS — must happen every loop iteration to prevent the
    // UDP receive buffer from filling under active captive-portal probing.
    _dns.processNextRequest();

    // Step 2: Advance the STA state machine.
    switch (_state) {
        case WifiState::CONNECTING:
            _tickConnecting();
            break;

        case WifiState::CONNECTED:
            _tickConnected();
            // Step 3: Update mDNS while STA is up.
            if (_mdnsRunning) {
                MDNS.update();
            }
            break;

        case WifiState::IDLE:
        case WifiState::FAILED:
            // Nothing to drive — AP keeps serving without STA.
            break;
    }
}

String WiFiManager::getMdnsHostname() const {
    if (_config) {
        return String(_config->systemConfig()["hostname"] | "elmahdyrelay");
    }
    return String(F("elmahdyrelay"));
}

bool WiFiManager::isMdnsEnabled() const {
    return _config ? (_config->systemConfig()["mdnsEnabled"] | true) : true;
}

String WiFiManager::getMdnsUrl() const {
    if (!isMdnsEnabled()) {
        return String();
    }
    return String(F("http://")) + getMdnsHostname() + F(".local");
}

void WiFiManager::refreshMdns() {
    if (isConnected()) {
        _registerMdns();
    } else {
        MDNS.close();
        _mdnsRunning = false;
        _mdnsHostname = getMdnsHostname();
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Public: connectSTA()
 * ───────────────────────────────────────────────────────────────────────────── */
void WiFiManager::connectSTA(const char* ssid, const char* password) {
    if (!ssid || ssid[0] == '\0') {
        Serial.println(F("[WiFi] connectSTA: empty SSID rejected"));
        return;
    }

    Serial.print(F("[WiFi] connectSTA: "));
    Serial.println(ssid);

    // Treat this as a completely fresh attempt; reset retry counter.
    _retryCount      = 0;
    _pendingSsid     = String(ssid);
    _pendingPassword = String(password ? password : "");
    _hadConnection   = false;  // Credentials not yet saved for this new set.

    _beginConnect();
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Public: scan()
 * ───────────────────────────────────────────────────────────────────────────── */
void WiFiManager::scan() {
    // Fire-and-forget: starts an async scan and returns immediately.
    // The caller must poll getScanResults() or WiFi.scanComplete() for completion.
    // Never call delay() here — this runs inside an ESPAsyncWebServer callback.
    WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false);
    Serial.println(F("[WiFi] Async scan started"));
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Public: getScanResults()
 * ───────────────────────────────────────────────────────────────────────────── */
String WiFiManager::getScanResults() const {
    int8_t count = static_cast<int8_t>(WiFi.scanComplete());

    // WIFI_SCAN_RUNNING (-1) or WIFI_SCAN_FAILED (-2): return empty array.
    if (count < 0) {
        return F("[]");
    }

    if (count == 0) {
        return F("[]");
    }

    // Build an index array and insertion-sort by RSSI descending (strongest first).
    // Maximum expected networks is small (≤ 20), so O(N²) is fine.
    static uint8_t idx[32];  // Static: avoids stack allocation; N never > 32.
    const uint8_t n = (count > 32) ? 32 : static_cast<uint8_t>(count);

    for (uint8_t i = 0; i < n; ++i) {
        idx[i] = i;
    }
    for (uint8_t i = 1; i < n; ++i) {
        uint8_t key = idx[i];
        int8_t  keyRssi = static_cast<int8_t>(WiFi.RSSI(key));
        int8_t  j = static_cast<int8_t>(i) - 1;
        while (j >= 0 && static_cast<int8_t>(WiFi.RSSI(idx[j])) < keyRssi) {
            idx[j + 1] = idx[j];
            --j;
        }
        idx[j + 1] = key;
    }

    // Serialise to JSON.  Each entry is ~50 bytes; budget 64 bytes per entry
    // plus 4-byte overhead for the array brackets.
    // ArduinoJson v7: JsonDocument is dynamically sized from the heap.
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    for (uint8_t i = 0; i < n; ++i) {
        uint8_t net = idx[i];
        JsonObject obj = arr.add<JsonObject>();
        obj["ssid"]   = WiFi.SSID(net);
        obj["rssi"]   = static_cast<int>(WiFi.RSSI(net));
        obj["secure"] = (WiFi.encryptionType(net) != ENC_TYPE_NONE);
    }

    String result;
    serializeJson(arr, result);
    return result;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Private: _buildApSsid()
 * ───────────────────────────────────────────────────────────────────────────── */
void WiFiManager::_buildApSsid() {
    if (_config) {
        const char* customApSsid = _config->wifiConfig()["apSsid"] | "";
        if (customApSsid[0] != '\0') {
            _apSsid = String(customApSsid);
            return;
        }
    }

    // WiFi.macAddress() → e.g. "A4:CF:12:AB:CD:EF"
    // Strip colons and take the last 4 hex characters → "CDEF".
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    // mac is now 12 hex chars (e.g. "A4CF12ABCDEF").
    // The last 4 chars are the suffix.
    String suffix = mac.substring(mac.length() - 4);
    suffix.toUpperCase();

    _apSsid = String(AP_SSID_PREFIX) + suffix;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Private: _startAp()
 * ───────────────────────────────────────────────────────────────────────────── */
void WiFiManager::_startAp() {
    // Retrieve the AP password from config (may have been user-changed).
    const char* apPassword = _config->wifiConfig()["apPassword"] | AP_PASSWORD;

    // Parse the AP_IP string into an IPAddress object.
    IPAddress apIp;
    apIp.fromString(AP_IP);

    // Configure the AP interface: IP, gateway (same as IP for /24 captive net),
    // and subnet mask.
    WiFi.softAPConfig(apIp, apIp, AP_SUBNET);

    // Bring up the AP.  softAP() returns true on success; log the outcome but
    // do not halt — the device can still serve over STA if AP init fails
    // (unlikely on genuine hardware).
    bool apOk = WiFi.softAP(_apSsid.c_str(), apPassword);
    if (!apOk) {
        Serial.println(F("[WiFi] WARNING: softAP() failed — captive portal unavailable"));
    }

    // Start the DNS server: wildcard '*' resolves to the AP IP so that any
    // connectivity-check host issued by a mobile captive-portal detector is
    // served by our web server on 192.168.4.1.
    _dns.setErrorReplyCode(DNSReplyCode::NoError);
    _dns.start(DNS_PORT, "*", apIp);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Private: _beginConnect()
 * ───────────────────────────────────────────────────────────────────────────── */
void WiFiManager::_beginConnect() {
    WiFi.disconnect(/*wifioff=*/false);

    // Apply static IP when requested; otherwise reset STA to DHCP.
    const JsonDocument& wifiDoc = _config->wifiConfig();
    const bool useDhcp = wifiDoc["dhcp"] | true;
    const char* staticIp = wifiDoc["staticIp"] | "";
    if (!useDhcp && strlen(staticIp) > 0) {
        IPAddress ip, gw, sn, dns1;
        ip.fromString(staticIp);
        gw.fromString(wifiDoc["gateway"] | "192.168.1.1");
        sn.fromString(wifiDoc["subnet"] | "255.255.255.0");
        dns1.fromString(wifiDoc["dns"] | "8.8.8.8");
        WiFi.config(ip, gw, sn, dns1);
        Serial.printf_P(PSTR("[WiFi] Static IP: %s gateway=%s dns=%s\n"),
                        staticIp,
                        (wifiDoc["gateway"] | "192.168.1.1"),
                        (wifiDoc["dns"] | "8.8.8.8"));
    } else {
        // Reset to DHCP in case a previous static config was applied.
        WiFi.config(IPAddress(), IPAddress(), IPAddress());
        Serial.println(F("[WiFi] DHCP enabled"));
    }

    Serial.printf_P(PSTR("[WiFi] Attempt %u/%u: connecting to '%s'\n"),
                    static_cast<unsigned>(_retryCount + 1),
                    static_cast<unsigned>(MAX_STA_RETRIES),
                    _pendingSsid.c_str());

    WiFi.begin(_pendingSsid.c_str(), _pendingPassword.c_str());

    _connectStart = millis();
    _state        = WifiState::CONNECTING;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Private: _tickConnecting()
 *
 * Called every loop() iteration while _state == CONNECTING.
 * ───────────────────────────────────────────────────────────────────────────── */
void WiFiManager::_tickConnecting() {
    wl_status_t status = WiFi.status();

    if (status == WL_CONNECTED) {
        // ── Success path ──────────────────────────────────────────────────────
        _state = WifiState::CONNECTED;
        _retryCount = 0;

        Serial.print(F("[WiFi] Connected. IP: "));
        Serial.println(WiFi.localIP());
        Serial.print(F("[WiFi] RSSI: "));
        Serial.print(WiFi.RSSI());
        Serial.println(F(" dBm"));

        // Save credentials to flash only on a freshly-supplied connectSTA()
        // call (not on auto-reconnect, where they are already persisted).
        if (!_hadConnection) {
            _saveCredentials();
            _hadConnection = true;
        }

        _registerMdns();

        // T037 — NTP: configure SNTP servers; sync happens asynchronously.
        // timezoneOffset is stored in minutes (e.g. 120 = UTC+2 for Egypt).
        {
            const JsonDocument& sys = _config->systemConfig();
            const int32_t ntpOffsetSec =
                static_cast<int32_t>(sys["timezoneOffset"] | 120) * 60;
            configTime(ntpOffsetSec, 0, "pool.ntp.org", "time.nist.gov");
            Serial.println(F("[WiFi] NTP sync started"));
        }

        if (_onConnected) {
            _onConnected();
        }
        return;
    }

    // ── Timeout check ─────────────────────────────────────────────────────────
    if ((millis() - _connectStart) >= WIFI_CONNECT_TIMEOUT_MS) {
        Serial.printf_P(PSTR("[WiFi] Attempt %u timed out (status=%d)\n"),
                        static_cast<unsigned>(_retryCount + 1),
                        static_cast<int>(status));

        WiFi.disconnect(/*wifioff=*/false);
        ++_retryCount;

        if (_retryCount < MAX_STA_RETRIES) {
            // Retry immediately — next _beginConnect() resets _connectStart.
            _beginConnect();
        } else {
            // All retries exhausted.
            Serial.println(F("[WiFi] All retries exhausted — staying in AP-only mode"));
            _state = WifiState::FAILED;
        }
    }
    // else: still within the timeout window; do nothing this tick.
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Private: _tickConnected()
 *
 * Called every loop() iteration while _state == CONNECTED.
 * Detects unexpected disconnections and initiates auto-reconnect.
 * ───────────────────────────────────────────────────────────────────────────── */
void WiFiManager::_tickConnected() {
    if (WiFi.status() == WL_CONNECTED) {
        return;  // Still healthy — nothing to do.
    }

    // Unexpected disconnect.
    Serial.println(F("[WiFi] STA link lost — initiating auto-reconnect"));
    if (_mdnsRunning) {
        MDNS.close();
        _mdnsRunning = false;
    }

    // Reload credentials from config for the reconnect attempt.
    const JsonDocument& wifiDoc = _config->wifiConfig();
    const char* storedSsid = wifiDoc["ssid"] | "";
    const char* storedPass = wifiDoc["password"] | "";

    if (storedSsid[0] == '\0') {
        // No credentials to reconnect with.
        Serial.println(F("[WiFi] No stored credentials for auto-reconnect"));
        _state = WifiState::FAILED;
        return;
    }

    _pendingSsid     = String(storedSsid);
    _pendingPassword = String(storedPass);
    _retryCount      = 0;
    _hadConnection   = true;  // Credentials already saved; skip re-save on success.

    _beginConnect();
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Private: _saveCredentials()
 * ───────────────────────────────────────────────────────────────────────────── */
void WiFiManager::_saveCredentials() {
    JsonDocument& wifiDoc = _config->wifiConfigMut();
    wifiDoc["ssid"]     = _pendingSsid;
    wifiDoc["password"] = _pendingPassword;

    if (_config->saveWifi()) {
        Serial.println(F("[WiFi] Credentials saved to LittleFS"));
    } else {
        Serial.println(F("[WiFi] WARNING: Failed to save credentials to LittleFS"));
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Private: _registerMdns()
 *
 * Stub: registers "elmahdy-relay.local" as the mDNS hostname.
 * The real hostname will be read from systemConfig()["hostname"] once T024
 * wires in LanguageManager and the system config screen is live.
 * ───────────────────────────────────────────────────────────────────────────── */
void WiFiManager::_registerMdns() {
    _mdnsRunning = false;
    _mdnsHostname = getMdnsHostname();

    if (!isMdnsEnabled()) {
        MDNS.close();
        Serial.println(F("[WiFi] mDNS disabled"));
        return;
    }

    MDNS.close();
    const char* hostname = _mdnsHostname.c_str();

    if (MDNS.begin(hostname)) {
        MDNS.addService("http", "tcp", 80);
        _mdnsRunning = true;
        Serial.printf_P(PSTR("[WiFi] mDNS: http://%s.local\n"), hostname);
    } else {
        Serial.println(F("[WiFi] WARNING: mDNS.begin() failed"));
    }
}
