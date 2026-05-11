/*
 * config_manager.cpp — LittleFS-backed JSON configuration with CRC32 atomic writes
 *
 * Target: ESP8266 (ESP-12F / NodeMCU), Arduino Core 3.x, C++17
 * Build:  PlatformIO (espressif8266)
 *
 * Write pattern (atomic):
 *   1. Serialise doc (without "crc") to String.
 *   2. Compute CRC32 over that String.
 *   3. Add "crc" field to doc and serialise again to <file>.tmp.
 *   4. Flush and close .tmp.
 *   5. Delete the live <file> (if it exists).
 *   6. Rename <file>.tmp → <file>.
 *
 * Read pattern:
 *   1. Open <file>.
 *   2. Deserialise into JsonDocument.
 *   3. Extract and remove "crc" field from the in-memory doc.
 *   4. Re-serialise doc (now without "crc") → compute CRC32.
 *   5. Compare computed CRC against stored value.
 *   6. If mismatch → load factory defaults for this section only.
 *
 * Note on CRC input: The "crc" field is excluded from the hash to avoid the
 * chicken-and-egg problem.  Both writer and reader must therefore strip the
 * field before hashing.  The writer adds the field back before the final
 * serialisation to disk; the reader removes it after parsing.
 */

#include "config_manager.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

bool ConfigManager::begin() {
    if (!LittleFS.begin()) {
        Serial.println(F("[CFG] FATAL: LittleFS mount failed"));
        return false;
    }
    Serial.println(F("[CFG] LittleFS mounted"));
    readAll();
    return true;
}

void ConfigManager::readAll() {
    // Each readSection() call loads factory defaults internally if the file is
    // missing or CRC-invalid.  Calling it with the section-specific doc means
    // only the affected section reverts — the rest are untouched.

    if (!readSection(CFG_WIFI_PATH, _wifi)) {
        Serial.println(F("[CFG] wifi.json missing/corrupt — using defaults"));
        _defaultWifi();
    }
    if (!readSection(CFG_MQTT_PATH, _mqtt)) {
        Serial.println(F("[CFG] mqtt.json missing/corrupt — using defaults"));
        _defaultMqtt();
    }
    if (!readSection(CFG_RELAYS_PATH, _relays)) {
        Serial.println(F("[CFG] relays.json missing/corrupt — using defaults"));
        _defaultRelays();
    }
    if (!readSection(CFG_RELAY_STATE_PATH, _relayState)) {
        Serial.println(F("[CFG] relays_state.json missing/corrupt — using defaults"));
        _defaultRelayState();
    }
    if (!readSection(CFG_TIMERS_PATH, _timers)) {
        Serial.println(F("[CFG] timers.json missing/corrupt — using defaults"));
        _defaultTimers();
    }
    if (!readSection(CFG_SCENES_PATH, _scenes)) {
        Serial.println(F("[CFG] scenes.json missing/corrupt — using defaults"));
        _defaultScenes();
    }
    if (!readSection(CFG_SYSTEM_PATH, _system)) {
        Serial.println(F("[CFG] system.json missing/corrupt — using defaults"));
        _defaultSystem();
    }

    Serial.println(F("[CFG] All config sections loaded"));
}

/* =========================================================================
 * Low-level I/O
 * ========================================================================= */

bool ConfigManager::write(const char* filename, JsonDocument& doc) {
    // Step 1: Remove any stale "crc" field so the CRC is computed over clean data.
    doc.remove("crc");

    // Step 2: Serialise to String (without "crc") and compute CRC32.
    String body;
    body.reserve(512);
    serializeJson(doc, body);
    const uint32_t crc = _crc32(reinterpret_cast<const uint8_t*>(body.c_str()),
                                  body.length());

    // Step 3: Add the CRC field to doc and serialise the final version.
    doc["crc"] = crc;

    String tmpPath = _tmpPath(filename);

    File f = LittleFS.open(tmpPath.c_str(), "w");
    if (!f) {
        Serial.printf("[CFG] ERROR: cannot open %s for write\n", tmpPath.c_str());
        return false;
    }

    size_t written = serializeJson(doc, f);
    f.flush();
    f.close();

    if (written == 0) {
        Serial.printf("[CFG] ERROR: zero bytes written to %s\n", tmpPath.c_str());
        LittleFS.remove(tmpPath.c_str());
        return false;
    }

    // Step 4: Atomically replace the live file.
    if (LittleFS.exists(filename)) {
        if (!LittleFS.remove(filename)) {
            Serial.printf("[CFG] ERROR: cannot remove old %s\n", filename);
            LittleFS.remove(tmpPath.c_str());
            return false;
        }
    }

    if (!LittleFS.rename(tmpPath.c_str(), filename)) {
        Serial.printf("[CFG] ERROR: rename %s → %s failed\n",
                      tmpPath.c_str(), filename);
        // .tmp is left in place; caller may retry.
        return false;
    }

    Serial.printf("[CFG] Saved %s (%u bytes, crc=0x%08X)\n",
                  filename, written, crc);
    return true;
}

bool ConfigManager::readSection(const char* filename, JsonDocument& doc) {
    if (_readAndValidateSectionFile(filename, doc)) {
        return true;
    }

    String tmpPath = _tmpPath(filename);
    if (!LittleFS.exists(tmpPath.c_str())) {
        return false;
    }

    if (!_readAndValidateSectionFile(tmpPath.c_str(), doc)) {
        return false;
    }

    Serial.printf("[CFG] Recovering %s from interrupted write %s\n",
                  filename, tmpPath.c_str());
    LittleFS.remove(filename);
    if (!LittleFS.rename(tmpPath.c_str(), filename)) {
        Serial.printf("[CFG] WARNING: recovered %s in RAM, rename failed\n",
                      filename);
    }
    return true;
}

bool ConfigManager::_readAndValidateSectionFile(const char* filename,
                                                JsonDocument& doc) {
    if (!LittleFS.exists(filename)) {
        return false;
    }

    File f = LittleFS.open(filename, "r");
    if (!f) {
        Serial.printf("[CFG] ERROR: cannot open %s for read\n", filename);
        return false;
    }

    doc.clear();
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Serial.printf("[CFG] ERROR: JSON parse failed for %s: %s\n",
                      filename, err.c_str());
        return false;
    }

    // Extract and remove the stored CRC before re-hashing.
    if (!doc["crc"].is<uint32_t>()) {
        Serial.printf("[CFG] ERROR: no crc field in %s\n", filename);
        return false;
    }
    const uint32_t storedCrc = doc["crc"].as<uint32_t>();
    doc.remove("crc");

    // Re-serialise (without "crc") and compute CRC32.
    const uint32_t computedCrc = _computeDocCrc(doc);

    if (computedCrc != storedCrc) {
        Serial.printf("[CFG] CRC mismatch in %s: stored=0x%08X computed=0x%08X\n",
                      filename, storedCrc, computedCrc);
        doc.clear();
        return false;
    }

    // "crc" has been removed from the in-memory doc — that is intentional.
    // It will be added back by write() when the section is next persisted.
    return true;
}

/* =========================================================================
 * Reset helpers
 * ========================================================================= */

void ConfigManager::resetSection(const char* filename) {
    if (LittleFS.exists(filename)) {
        LittleFS.remove(filename);
    }

    // Reload factory defaults into the matching in-memory doc.
    if      (strcmp(filename, CFG_WIFI_PATH)        == 0) { _defaultWifi();       }
    else if (strcmp(filename, CFG_MQTT_PATH)        == 0) { _defaultMqtt();       }
    else if (strcmp(filename, CFG_RELAYS_PATH)      == 0) { _defaultRelays();     }
    else if (strcmp(filename, CFG_RELAY_STATE_PATH) == 0) { _defaultRelayState(); }
    else if (strcmp(filename, CFG_TIMERS_PATH)      == 0) { _defaultTimers();     }
    else if (strcmp(filename, CFG_SCENES_PATH)      == 0) { _defaultScenes();     }
    else if (strcmp(filename, CFG_SYSTEM_PATH)      == 0) { _defaultSystem();     }
    else {
        Serial.printf("[CFG] resetSection: unknown filename %s\n", filename);
    }
}

void ConfigManager::resetAll() {
    const char* files[] = {
        CFG_WIFI_PATH, CFG_MQTT_PATH, CFG_RELAYS_PATH,
        CFG_RELAY_STATE_PATH, CFG_TIMERS_PATH,
        CFG_SCENES_PATH, CFG_SYSTEM_PATH
    };
    for (const char* f : files) {
        if (LittleFS.exists(f)) {
            LittleFS.remove(f);
        }
    }
    _defaultWifi();
    _defaultMqtt();
    _defaultRelays();
    _defaultRelayState();
    _defaultTimers();
    _defaultScenes();
    _defaultSystem();
    Serial.println(F("[CFG] Factory reset complete — all defaults loaded"));
}

/* =========================================================================
 * Per-section save
 * ========================================================================= */

bool ConfigManager::saveWifi()       { return write(CFG_WIFI_PATH,        _wifi);       }
bool ConfigManager::saveMqtt()       { return write(CFG_MQTT_PATH,        _mqtt);       }
bool ConfigManager::saveRelays()     { return write(CFG_RELAYS_PATH,      _relays);     }
bool ConfigManager::saveRelayState() { return write(CFG_RELAY_STATE_PATH, _relayState); }
bool ConfigManager::saveTimers()     { return write(CFG_TIMERS_PATH,      _timers);     }
bool ConfigManager::saveScenes()     { return write(CFG_SCENES_PATH,      _scenes);     }
bool ConfigManager::saveSystem()     { return write(CFG_SYSTEM_PATH,      _system);     }

/* =========================================================================
 * Factory-default loaders
 * ========================================================================= */

void ConfigManager::_defaultWifi() {
    _wifi.clear();
    _wifi["ssid"]       = "";
    _wifi["password"]   = "";
    _wifi["dhcp"]       = true;
    _wifi["staticIp"]   = "192.168.1.50";
    _wifi["gateway"]    = "192.168.1.1";
    _wifi["subnet"]     = "255.255.255.0";
    _wifi["dns"]        = "8.8.8.8";
    _wifi["apPassword"] = AP_PASSWORD;  // "12345678" from config.h
}

void ConfigManager::_defaultMqtt() {
    _mqtt.clear();
    _mqtt["enabled"]  = true;
    _mqtt["broker"]   = "broker.emqx.io";
    _mqtt["port"]     = 1883;
    _mqtt["username"] = "";
    _mqtt["password"] = "";
    _mqtt["prefix"]   = "elmahdy";
}

void ConfigManager::_defaultRelays() {
    _relays.clear();
    _relays["channelCount"] = DEFAULT_CHANNEL_COUNT;  // 2 from config.h

    JsonArray channels = _relays["channels"].to<JsonArray>();

    // Channel 1 — GPIO 5 (D1) — "قناة 1"
    JsonObject ch1 = channels.add<JsonObject>();
    ch1["id"]             = 1;
    ch1["pin"]            = RELAY_1_PIN;   // 5
    ch1["name"]           = "\xD9\x82\xD9\x86\xD8\xA7\xD8\xA9 1";  // "قناة 1" UTF-8
    ch1["powerOnState"]   = "last";
    ch1["pulseDuration"]  = 0;
    ch1["interlockGroup"] = 0;

    // Channel 2 — GPIO 4 (D2) — "قناة 2"
    JsonObject ch2 = channels.add<JsonObject>();
    ch2["id"]             = 2;
    ch2["pin"]            = RELAY_2_PIN;   // 4
    ch2["name"]           = "\xD9\x82\xD9\x86\xD8\xA7\xD8\xA9 2";  // "قناة 2" UTF-8
    ch2["powerOnState"]   = "last";
    ch2["pulseDuration"]  = 0;
    ch2["interlockGroup"] = 0;
}

void ConfigManager::_defaultRelayState() {
    _relayState.clear();
    JsonArray states = _relayState["states"].to<JsonArray>();
    // Pre-populate 4 slots (all OFF) to cover the max channel count.
    for (uint8_t i = 0; i < MAX_CHANNELS; ++i) {
        states.add(false);
    }
}

void ConfigManager::_defaultTimers() {
    _timers.clear();
    _timers["nextId"] = 1;
    _timers["timers"].to<JsonArray>();  // empty array
}

void ConfigManager::_defaultScenes() {
    _scenes.clear();
    _scenes["scenes"].to<JsonArray>();  // empty array
}

void ConfigManager::_defaultSystem() {
    _system.clear();
    _system["buzzerEnabled"]   = true;
    _system["ledEnabled"]      = true;
    // Disabled by default because RESET_PIN is GPIO16 on ESP8266. GPIO16 has
    // no internal pull-up, so an unwired reset input can float low under relay
    // noise and look like a long factory-reset press.
    _system["resetEnabled"]    = false;
    _system["buzzerPin"]       = BUZZER_PIN;  // 13
    _system["resetPin"]        = RESET_PIN;   // 16
    _system["hostname"]        = "elmahdyrelay";
    _system["mdnsEnabled"]     = true;
    _system["language"]        = "ar";
    _system["timezoneOffset"]  = 120;
}

/* =========================================================================
 * CRC32 implementation  (ISO 3309 / Ethernet — poly 0xEDB88320)
 *
 * This is the same polynomial used by zlib, LittleFS, and most hardware CRC
 * peripherals, so future hardware acceleration is a drop-in replacement.
 *
 * The "bit-by-bit" approach with a 256-entry table is not used here to keep
 * flash consumption low; the nibble-at-a-time approach halves the table size
 * with minimal speed penalty on the 80 MHz Xtensa core.
 * ========================================================================= */

// 16-entry lookup table for nibble-at-a-time CRC32.
static const uint32_t CRC32_TABLE[16] = {
    0x00000000UL, 0x1DB71064UL, 0x3B6E20C8UL, 0x26D930ACUL,
    0x76DC4190UL, 0x6B6B51F4UL, 0x4DB26158UL, 0x5005713CUL,
    0xEDB88320UL, 0xF00F9344UL, 0xD6D6A3E8UL, 0xCB61B38CUL,
    0x9B64C2B0UL, 0x86D3D2D4UL, 0xA00AE278UL, 0xBDBDF21CUL
};

uint32_t ConfigManager::_crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFUL;
    for (size_t i = 0; i < len; ++i) {
        uint8_t byte = data[i];
        crc = (crc >> 4) ^ CRC32_TABLE[(crc ^ byte)        & 0x0FU];
        crc = (crc >> 4) ^ CRC32_TABLE[(crc ^ (byte >> 4)) & 0x0FU];
    }
    return crc ^ 0xFFFFFFFFUL;
}

uint32_t ConfigManager::_computeDocCrc(JsonDocument& doc) {
    // doc must already have "crc" removed before this is called.
    String body;
    body.reserve(512);
    serializeJson(doc, body);
    return _crc32(reinterpret_cast<const uint8_t*>(body.c_str()), body.length());
}

String ConfigManager::_tmpPath(const char* filename) {
    String path(filename);
    path += ".tmp";
    return path;
}
