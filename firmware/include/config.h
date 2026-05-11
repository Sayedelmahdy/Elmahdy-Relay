/*
 * config.h — Compile-time constants, pin definitions, and version macro
 *
 * Target: ESP8266 (ESP-12F / NodeMCU), Arduino Core 3.x, C++17
 * Build:  PlatformIO (espressif8266)
 *
 * All values here are defaults. Runtime-configurable values are stored
 * in LittleFS JSON files managed by config_manager.h/.cpp.
 */

#ifndef CONFIG_H_
#define CONFIG_H_

/* -------------------------------------------------------------------------
 * Firmware version
 * Overridable at build time via -DVERSION=\"x.y.z\" in platformio.ini.
 * ------------------------------------------------------------------------- */
#ifndef VERSION
#define VERSION "1.0.0"
#endif

/* -------------------------------------------------------------------------
 * GPIO pin assignments (NodeMCU / Wemos D1 Mini silk-screen labels)
 *
 * Boot-safety note: GPIOs 0, 2, and 15 affect the ESP8266 boot mode when
 * driven LOW at power-on.  See BOOT_SENSITIVE_GPIOS below.  The relay and
 * peripheral pins chosen here (4, 5, 12, 13, 14, 16) are all boot-safe.
 * ------------------------------------------------------------------------- */
#define RELAY_1_PIN   5   // D1 — channel 1 relay output
#define RELAY_2_PIN   4   // D2 — channel 2 relay output
#define RELAY_3_PIN  14   // D5 — channel 3 relay output
#define RELAY_4_PIN  12   // D6 — channel 4 relay output
#define BUZZER_PIN   13   // D7 — active buzzer
#define RESET_PIN    16   // D0 — factory-reset / long-press button
#define LED_PIN       2   // D4 — built-in LED (active-LOW on most modules)

/* -------------------------------------------------------------------------
 * Relay output polarity
 *
 * true  = active-LOW  (LOW  signal → relay coil energised → contacts close)
 * false = active-HIGH (HIGH signal → relay coil energised → contacts close)
 *
 * Most optically-isolated relay modules sold for ESP8266 are active-LOW.
 * Overridable via -DRELAY_ACTIVE_LOW=false in platformio.ini.
 * ------------------------------------------------------------------------- */
#ifndef RELAY_ACTIVE_LOW
#define RELAY_ACTIVE_LOW true
#endif

/* -------------------------------------------------------------------------
 * Boot-sensitive GPIO list
 *
 * These GPIOs must not be driven to a conflicting logic level at power-on
 * or the ESP8266 will enter the wrong boot mode (UART download instead of
 * normal SPI flash boot).
 *
 *   GPIO 0  — must be HIGH (or floating) for normal boot
 *   GPIO 2  — must be HIGH for normal boot (also built-in LED on most boards)
 *   GPIO 15 — must be LOW  for normal boot
 *
 * relay_controller.cpp uses this list to reject unsafe GPIO assignments.
 * ------------------------------------------------------------------------- */
static const uint8_t BOOT_SENSITIVE_GPIOS[] = {0, 2, 15};
static const uint8_t BOOT_SENSITIVE_GPIO_COUNT =
    static_cast<uint8_t>(sizeof(BOOT_SENSITIVE_GPIOS) /
                         sizeof(BOOT_SENSITIVE_GPIOS[0]));

/* -------------------------------------------------------------------------
 * AP / captive-portal defaults
 *
 * The SSID suffix (last 4 hex digits of the chip ID) is appended at runtime
 * by wifi_manager.cpp to produce e.g. "ElmahdyRelay_A3F1".
 * ------------------------------------------------------------------------- */
#define AP_SSID_PREFIX  "ElmahdyRelay_"
#define AP_PASSWORD     "12345678"
#define AP_IP           "192.168.4.1"

/* -------------------------------------------------------------------------
 * Capacity limits
 *
 * These compile-time ceilings drive static array sizes and JSON buffer
 * allocations throughout the firmware.  Increasing them costs RAM; the
 * 80 KB usable heap on ESP8266 is the hard constraint.
 * ------------------------------------------------------------------------- */
#define MAX_CHANNELS    4    // Maximum relay channels (1–4 configurable)
#define MAX_TIMERS      8    // Maximum concurrent countdown/scheduled timers
#define MAX_SCENES     10    // Maximum stored scene presets
#define MAX_WS_CLIENTS  4    // Maximum concurrent WebSocket connections
#define RELAY_STAGGER_MS 50  // ms between relay switching in bulk mode (prevents surge)

/* -------------------------------------------------------------------------
 * Runtime defaults
 *
 * DEFAULT_CHANNEL_COUNT: number of relay channels enabled after factory
 * reset, before the user reconfigures via the web UI.
 * ------------------------------------------------------------------------- */
#define DEFAULT_CHANNEL_COUNT  2

#endif /* CONFIG_H_ */
