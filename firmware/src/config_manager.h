/*
 * config_manager.h — LittleFS-backed JSON configuration with CRC32 atomic writes
 *
 * Target: ESP8266 (ESP-12F / NodeMCU), Arduino Core 3.x, C++17
 * Build:  PlatformIO (espressif8266)
 *
 * Design contract:
 *   - All 7 config sections are held in RAM as JsonDocument objects.
 *   - Writes are atomic: serialize → tmp file → CRC32 → rename.
 *   - Reads verify CRC32; on mismatch the section silently reverts to factory
 *     defaults so a single corrupt file never bricks the device.
 *   - No malloc after init: JsonDocument uses internal heap only during
 *     read/write operations, not in steady-state hot paths.
 *   - No delay() calls anywhere in this module.
 */

#ifndef CONFIG_MANAGER_H_
#define CONFIG_MANAGER_H_

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

#include "config.h"

/* -------------------------------------------------------------------------
 * LittleFS file paths
 * ------------------------------------------------------------------------- */
static const char* const CFG_WIFI_PATH         = "/wifi.json";
static const char* const CFG_MQTT_PATH         = "/mqtt.json";
static const char* const CFG_RELAYS_PATH       = "/relays.json";
static const char* const CFG_RELAY_STATE_PATH  = "/relays_state.json";
static const char* const CFG_TIMERS_PATH       = "/timers.json";
static const char* const CFG_SCENES_PATH       = "/scenes.json";
static const char* const CFG_SYSTEM_PATH       = "/system.json";

/* -------------------------------------------------------------------------
 * Per-section JSON document capacity (bytes)
 *
 * ArduinoJson v7 JsonDocument is a single heap allocation.  These are sized
 * conservatively above the maximum serialised lengths given in data-model.md.
 * Keeping them as tight as possible is important on the 80 KB ESP8266 heap.
 * ------------------------------------------------------------------------- */
static const size_t CFG_WIFI_DOC_CAP        = 512;
static const size_t CFG_MQTT_DOC_CAP        = 384;
static const size_t CFG_RELAYS_DOC_CAP      = 768;
static const size_t CFG_RELAY_STATE_DOC_CAP = 128;
static const size_t CFG_TIMERS_DOC_CAP      = 1536;
static const size_t CFG_SCENES_DOC_CAP      = 1536;
static const size_t CFG_SYSTEM_DOC_CAP      = 512;

/* -------------------------------------------------------------------------
 * ConfigManager
 * ------------------------------------------------------------------------- */
class ConfigManager {
public:
    ConfigManager() = default;
    ~ConfigManager() = default;

    /* Lifecycle ------------------------------------------------------------- */

    /**
     * Mount LittleFS and load all 7 config sections into RAM.
     * Returns false if LittleFS fails to mount (fatal — caller should halt).
     */
    bool begin();

    /**
     * Read and CRC-validate every section.  Sections that fail validation are
     * replaced with factory defaults.  Called automatically by begin().
     */
    void readAll();

    /* Low-level I/O --------------------------------------------------------- */

    /**
     * Atomic write: serialise doc to <filename>.tmp, compute CRC32 over the
     * serialised bytes, inject the "crc" field, flush, delete the live file,
     * rename .tmp to live.  Returns true on success.
     *
     * The doc is modified in-place to include the computed "crc" field before
     * it is written, so callers can rely on the in-memory copy staying
     * consistent with what is on disk.
     */
    bool write(const char* filename, JsonDocument& doc);

    /**
     * Open filename, deserialise into doc, verify the embedded "crc" field
     * against the CRC32 of the JSON string with the "crc" field stripped.
     * Returns true if the file exists and the CRC is valid.
     */
    bool readSection(const char* filename, JsonDocument& doc);

    /* Per-section reset ----------------------------------------------------- */

    /** Delete filename from LittleFS and reload factory defaults for that
     *  section into the corresponding in-memory document. */
    void resetSection(const char* filename);

    /** Delete all 7 config files and reload all factory defaults. */
    void resetAll();

    /* Per-section save ------------------------------------------------------ */
    bool saveWifi();
    bool saveMqtt();
    bool saveRelays();
    bool saveRelayState();
    bool saveTimers();
    bool saveScenes();
    bool saveSystem();

    /* Accessors (const — callers must use save*() to persist changes) ------- */
    const JsonDocument& wifiConfig()       const { return _wifi;       }
    const JsonDocument& mqttConfig()       const { return _mqtt;       }
    const JsonDocument& relayConfig()      const { return _relays;     }
    const JsonDocument& relayStateConfig() const { return _relayState; }
    const JsonDocument& timerConfig()      const { return _timers;     }
    const JsonDocument& sceneConfig()      const { return _scenes;     }
    const JsonDocument& systemConfig()     const { return _system;     }

    /* Mutable accessors — use sparingly; always follow with save*() --------- */
    JsonDocument& wifiConfigMut()       { return _wifi;       }
    JsonDocument& mqttConfigMut()       { return _mqtt;       }
    JsonDocument& relayConfigMut()      { return _relays;     }
    JsonDocument& relayStateConfigMut() { return _relayState; }
    JsonDocument& timerConfigMut()      { return _timers;     }
    JsonDocument& sceneConfigMut()      { return _scenes;     }
    JsonDocument& systemConfigMut()     { return _system;     }

private:
    /* In-memory documents --------------------------------------------------- */
    JsonDocument _wifi;
    JsonDocument _mqtt;
    JsonDocument _relays;
    JsonDocument _relayState;
    JsonDocument _timers;
    JsonDocument _scenes;
    JsonDocument _system;

    /* Factory-default loaders ----------------------------------------------- */
    void _defaultWifi();
    void _defaultMqtt();
    void _defaultRelays();
    void _defaultRelayState();
    void _defaultTimers();
    void _defaultScenes();
    void _defaultSystem();

    /* Helpers --------------------------------------------------------------- */

    /**
     * Compute CRC32 (ISO 3309 / Ethernet polynomial 0xEDB88320) over an
     * arbitrary byte buffer.  Pure software — no hardware peripheral assumed.
     */
    static uint32_t _crc32(const uint8_t* data, size_t len);

    /**
     * Serialise doc to a String, strip the "crc" member, and compute CRC32
     * over that stripped string.  This is the canonical CRC input used for
     * both writing and verification.
     *
     * Stripping "crc" before hashing means the stored CRC field itself is not
     * part of the protected data, which avoids the chicken-and-egg problem of
     * needing to know the CRC to compute the CRC.
    */
    static uint32_t _computeDocCrc(JsonDocument& doc);
    static bool _readAndValidateSectionFile(const char* filename,
                                            JsonDocument& doc);

    /** Build the .tmp path for a given canonical filename, e.g.
     *  "/wifi.json" → "/wifi.json.tmp" */
    static String _tmpPath(const char* filename);
};

/* -------------------------------------------------------------------------
 * Global singleton — extern declaration; definition is in config_manager.cpp
 * ------------------------------------------------------------------------- */
extern ConfigManager configManager;

#endif /* CONFIG_MANAGER_H_ */
