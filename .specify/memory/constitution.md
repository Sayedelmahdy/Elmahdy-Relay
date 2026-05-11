<!--
  SYNC IMPACT REPORT
  ====================
  Version change: 0.0.0 (template) → 1.0.0
  Modified principles: N/A (initial creation from template)
  Added sections:
    - 7 Core Principles (Production-First, Minimal Footprint Architecture,
      Dual-Mode WiFi, Configurable Channel Architecture,
      Resilient Configuration Storage, Full Bilingual Support,
      MQTT Integration)
    - Technology Stack
    - Feature Set
    - API Design
    - Code Standards
    - Build & Distribution
    - Security
    - Quality Standards
  Removed sections: None
  Templates requiring updates:
    - .specify/templates/plan-template.md — ✅ reviewed, no updates needed
    - .specify/templates/spec-template.md — ✅ reviewed, no updates needed
    - .specify/templates/tasks-template.md — ✅ reviewed, no updates needed
  Follow-up TODOs: None
-->

# Elmahdy Relay Constitution

## Core Principles

### I. Production-First (NON-NEGOTIABLE)

This is a commercial product sold to end-users. Every decision MUST
prioritize reliability.

- Device MUST never lose configuration under any circumstance.
- All settings MUST persist to LittleFS with corruption-safe write
  patterns (write-to-temp, verify CRC, rename-to-active).
- Hardware reset button MUST always restore factory defaults.
- Hardware watchdog timer (WDT) MUST be enabled; auto-reboots on
  firmware hang.
- Dual-write config with CRC32 validation on every read.
- Relay GPIOs MUST avoid boot-glitch pins (GPIO 0, 2, 15) by default
  to prevent relay flicker during power-on.
- Firmware binary MUST remain under 500KB to allow OTA dual-bank
  updates on 4MB flash.

### II. Minimal Footprint Architecture

Every byte matters on ESP8266. The firmware and web assets MUST fit
within strict flash and RAM budgets.

- ESPAsyncWebServer for non-blocking, low-memory web serving.
- WebSocket for real-time UI updates (built into ESPAsyncWebServer —
  zero extra binary cost).
- LittleFS for config and web asset storage.
- All HTML/CSS/JS MUST be minified and GZIP-compressed before
  deployment to LittleFS.
- No external CDN dependencies — device MUST work 100% offline on
  local network.
- No heavy JS frameworks — vanilla ES5 JavaScript only.
- Compiler optimization: `-Os` (optimize for size), strip debug
  symbols in production builds.
- No blocking `delay()` calls — all timing MUST use `millis()`-based
  non-blocking patterns.

### III. Dual-Mode WiFi (NON-NEGOTIABLE)

The device MUST run AP + STA simultaneously at all times to guarantee
user access regardless of router availability.

- AP defaults: SSID = `ElmahdyRelay_XXXX` (last 4 hex chars of MAC),
  Password = `12345678`, IP = `192.168.4.1`.
- If router WiFi is unavailable, the AP MUST ensure the device stays
  controllable.
- Captive portal MUST activate on first boot or when no WiFi is
  configured.
- STA mode connects to user's home router for internet-dependent
  features (MQTT, NTP).

### IV. Configurable Channel Architecture

The device MUST support 1 to 4 relay channels, fully user-configurable
from the web UI.

Each channel MUST have:
- Configurable GPIO pin assignment.
- User-defined name (Arabic or English, max 20 characters).
- Independent ON/OFF state.
- Timer/countdown association.
- Pulse/inching mode (relay ON for configured duration, then auto-OFF).
- Scene membership.
- Power-on state behavior: Last State / Always ON / Always OFF.
- Interlock group assignment (only one relay in a group can be ON at
  a time — for motor/curtain safety).

Safe default GPIOs: 5 (D1), 4 (D2), 14 (D5), 12 (D6). Firmware MUST
warn if user selects boot-sensitive pins (GPIO 0, 2, 15).

### V. Resilient Configuration Storage (NON-NEGOTIABLE)

All configuration MUST be stored in LittleFS as JSON files with
corruption-safe write patterns.

- Dual-file write pattern: write to `.tmp` → verify CRC32 → rename
  to `.json`. Active config file is NEVER written directly.
- Separate config files: `wifi.json`, `mqtt.json`, `relays.json`,
  `timers.json`, `scenes.json`, `system.json`.
- On boot: validate each config file independently. If corrupt, load
  factory defaults for THAT section only — never wipe all settings.
- Factory defaults MUST be compiled into firmware as `constexpr`
  fallback constants.
- Backup/restore: export all config as single JSON download; import
  from JSON upload for device cloning or recovery.

### VI. Full Bilingual Support (Arabic & English)

Every UI string, label, button, message, error, and notification MUST
exist in both Arabic and English.

- Language strings stored as JSON language packs in LittleFS:
  `lang_ar.json`, `lang_en.json`.
- Arabic mode MUST enable full RTL layout (CSS `direction: rtl`).
- Language MUST be switchable from UI header, persisted to config.
- Default language: Arabic.
- All features MUST work identically in both languages.

### VII. MQTT Integration

The device MUST support MQTT for remote control and monitoring with
automatic Home Assistant discovery.

- Default broker: `broker.hivemq.com`, port `1883`. All configurable.
- Topic structure:
  - `{prefix}/relay/{channel}/control` — receive ON/OFF/TOGGLE
  - `{prefix}/relay/{channel}/status` — publish state
  - `{prefix}/relay/all/control` — control all channels
  - `{prefix}/relay/all/status` — all channel states
  - `{prefix}/system/status` — LWT online/offline
  - `{prefix}/system/info` — version, uptime, RSSI, IP
  - `{prefix}/timer/{id}/control` — timer control
  - `{prefix}/scene/{name}/control` — scene activation
- Auto-reconnect with exponential backoff.
- Last Will and Testament (LWT) for offline detection.
- Home Assistant MQTT Auto-Discovery: publish HA-compatible discovery
  messages on boot for zero-config detection.

## Technology Stack

- **Firmware**: C++ with Arduino Core for ESP8266, PlatformIO build
  system.
- **Web Server**: ESPAsyncWebServer + ESPAsyncTCP (non-blocking,
  low memory).
- **Real-Time Updates**: WebSocket (built into ESPAsyncWebServer) for
  instant UI state sync — no polling.
- **Filesystem**: LittleFS for config storage and web UI assets.
- **JSON**: ArduinoJson v7.
- **MQTT Client**: PubSubClient or AsyncMqttClient.
- **NTP**: Built-in `configTime()` or NTPClient library.
- **mDNS**: ESP8266mDNS (built-in).
- **OTA**: ArduinoOTA or HTTP OTA update.
- **Web UI**: Vanilla HTML5 + CSS + ES5 JavaScript only — NO frameworks
  (no React, Angular, Vue, jQuery).
- **Target Hardware**: ESP8266 (ESP-12F / NodeMCU / Wemos D1 Mini
  compatible), 4MB flash.

### Default GPIO Pin Assignment (NodeMCU)

| Pin | GPIO | Function         |
|-----|------|------------------|
| D1  | 5    | Relay 1          |
| D2  | 4    | Relay 2          |
| D5  | 14   | Relay 3          |
| D6  | 12   | Relay 4          |
| D7  | 13   | Buzzer           |
| D0  | 16   | Reset Button     |
| D4  | 2    | Built-in LED     |

All relay and buzzer GPIOs are user-configurable from the web UI.
No physical wall switch inputs — control is via Web UI, MQTT, and
PWA only.

## Feature Set

### Dashboard (Landing Page)

- Top navigation bar with Dashboard and Configuration tabs.
- Real-time relay state cards with toggle switches (one per channel),
  updated instantly via WebSocket.
- Channel names displayed (user-configured, Arabic/English).
- Quick "All ON" / "All OFF" buttons.
- WiFi signal strength (RSSI bars).
- Device uptime counter.
- MQTT connection status indicator.
- Firmware version display.
- Language toggle (AR/EN) in header.

### Configuration Pages

- **WiFi Settings**: SSID scan & select, password, static IP option,
  AP settings.
- **MQTT Settings**: broker, port, username, password, prefix,
  enable/disable.
- **Relay Settings**: channel count (1-4), GPIO pin per channel,
  channel names, power-on state per channel.
- **Timer Settings**: per-channel countdown, scheduled on/off with
  NTP, repeat daily/weekly/custom.
- **Scene Settings**: named multi-channel presets.
- **System Settings**: buzzer enable/disable, LED enable/disable,
  reset button enable/disable, device hostname/mDNS name, factory
  reset from UI, firmware version, reboot, OTA firmware update.
- **About Page**: "Elmahdy Relay" branding, © Sayed Elmahdy,
  Phone: 01093307397.

### Timer System

- Countdown timer: X minutes/hours auto-off or auto-on per channel.
- Scheduled timer: NTP-synced, ON/OFF at specific time.
- Repeat options: once, daily, weekdays, weekend, custom days.
- Timers MUST persist across power cycles.
- Maximum 8 timers.

### Scene System

- Named presets controlling multiple relay states simultaneously
  (e.g., "Sleep Mode" = CH1 OFF, CH2 OFF, CH3 ON).
- Triggerable from Dashboard, MQTT, and PWA.
- MUST persist across power cycles.
- Maximum 10 scenes.

### Buzzer & LED Feedback

- **Buzzer** (optional hardware, default GPIO13/D7):
  - Short beep on relay toggle.
  - Double-beep on WiFi connect.
  - Long beep on factory reset.
  - Anti-spam: minimum 100ms between buzzer activations.
- **Built-in LED** (GPIO2/D4):
  - Fast blink = AP mode only.
  - Slow blink = connected to WiFi.
  - Solid = MQTT connected.
- Both independently enable/disable from settings.

### Reset Button (default GPIO16/D0)

- Short press (<3s) = toggle relay 1 (or configurable).
- Long press (3-10s) = reboot device (buzzer confirms).
- Very long press (>10s) = factory reset all settings (buzzer rapid
  beep).
- GPIO configurable. Feature can be disabled in settings.

### PWA (Progressive Web App)

- Full manifest with app icon.
- Installable on mobile home screen.
- Minimal icon sizes: 48×48, 192×192.
- Offline-capable for local network control.
- Service worker for caching static assets.
- App name: "Elmahdy Relay".

### OTA Firmware Update

- Upload `.bin` file through web UI.
- Progress indicator during flash.
- Auto-reboot after successful update.
- Firmware size validation before flashing.
- MUST preserve all user configuration after update.

### Backup & Restore

- Export all device configuration as a single JSON file download.
- Import/restore configuration from JSON file upload.
- Useful for cloning settings to multiple devices or recovery after
  factory reset.

### Additional Production Features

- Watchdog timer (hardware WDT enabled, auto-reboot on hang).
- mDNS discovery (`elmahdyrelay.local`, hostname configurable).
- NTP sync (auto on WiFi connect, timezone configurable).
- Uptime counter (continuous on dashboard).
- WiFi RSSI display (signal strength bars on dashboard).
- Pulse/inching mode per channel.
- Interlock groups (mutual exclusion for motor/curtain safety).
- Power-on state per channel (Last State / Always ON / Always OFF).
- Home Assistant MQTT Auto-Discovery.

## API Design

All API endpoints return JSON responses with CORS headers
(`Access-Control-Allow-Origin: *`) so any external mobile app or
third-party application can consume them. The built-in web UI uses
the exact same API — it is not special.

### REST Endpoints

| Method | Path                    | Description                        |
|--------|-------------------------|------------------------------------|
| GET    | /api/info               | API version, device name, endpoints|
| GET    | /api/status             | All relay states + system info     |
| POST   | /api/relay/{ch}         | Control single relay               |
| POST   | /api/relay/all          | Control all relays                 |
| GET    | /api/config/{section}   | Read config section                |
| POST   | /api/config/{section}   | Update config section              |
| GET    | /api/timers             | List all timers                    |
| POST   | /api/timer              | Create/update timer                |
| DELETE | /api/timer/{id}         | Delete timer                       |
| GET    | /api/scenes             | List all scenes                    |
| POST   | /api/scene              | Create/update scene                |
| DELETE | /api/scene/{name}       | Delete scene                       |
| POST   | /api/system/reboot      | Reboot device                      |
| POST   | /api/system/reset       | Factory reset                      |
| POST   | /api/system/update      | OTA firmware upload                |
| GET    | /api/backup             | Download all config as JSON        |
| POST   | /api/restore            | Upload and restore config          |

### WebSocket

- **Endpoint**: `/ws`
- Real-time relay state push to all connected clients.
- Bidirectional: clients can send commands, server broadcasts state
  changes.

## Code Standards

### C++ (Firmware)

- Descriptive English variable/function names.
- Comments in English.
- Use `const` and `constexpr` wherever possible.
- No blocking `delay()` — use `millis()` non-blocking timers.
- Modular file structure with separate `.h`/`.cpp` pairs:
  `WiFiManager`, `MQTTManager`, `RelayController`, `TimerEngine`,
  `SceneManager`, `ConfigManager`, `WebServerHandler`,
  `BuzzerController`, `LEDController`, `ResetHandler`, `OTAHandler`,
  `LanguageManager`.
- Maximum 50 lines per function.

### JavaScript (Web UI)

- Vanilla ES5 for maximum browser compatibility.
- IIFE or module pattern (no global variable pollution).
- DOM manipulation for UI updates.
- WebSocket client for real-time state sync.
- All user-facing strings loaded from language endpoint.

### CSS (Styling)

- Mobile-first responsive design.
- CSS custom properties (variables) for theming.
- RTL support via `[dir="rtl"]` selectors.
- No CSS frameworks.
- Dark theme base (modern IoT aesthetic).

## Build & Distribution

- **Product Name**: Elmahdy Relay
- **Creator & Copyright**: © Sayed Elmahdy — All Rights Reserved
- **Contact Phone**: 01093307397
- **Distribution**: Single compiled `.bin` file flashed via Tasmotizer.

### Size Budgets

| Component      | Budget   |
|----------------|----------|
| Firmware binary | < 500KB |
| LittleFS image  | < 512KB |
| Total active    | < 1MB   |

### Flash Layout (4MB)

| Region       | Size   |
|--------------|--------|
| Sketch       | ~1MB   |
| OTA Buffer   | ~1MB   |
| LittleFS     | ~2MB   |

### Build Pipeline

1. Write C++ firmware.
2. Write HTML/CSS/JS web assets.
3. Minify: `html-minifier`, `csso`, `terser`.
4. GZIP compress all web assets.
5. Build LittleFS image.
6. PlatformIO compile firmware.
7. Merge firmware + LittleFS into single `.bin` for Tasmotizer.

## Security

- No authentication by default (local network trust model).
- Optional simple password protection for web UI (configurable).
- MQTT supports username/password authentication.
- No sensitive data transmitted in MQTT topics.
- OTA update requires local network access only (no cloud OTA).
- CORS enabled (`Access-Control-Allow-Origin: *`) on all `/api/`
  endpoints to allow external app access.

## Quality Standards

| Metric                         | Target           |
|--------------------------------|------------------|
| Boot to functional state       | < 5 seconds      |
| Web UI load time (local)       | < 2 seconds      |
| Relay response latency         | < 100ms          |
| Config loss on power failure   | Zero             |
| WebSocket state update latency | < 50ms           |

- Graceful degradation: device MUST be fully functional without
  internet (MQTT/NTP are optional enhancements).
- All features MUST work identically in Arabic and English.

## Governance

- This constitution is the **supreme authority** for all development
  decisions on Elmahdy Relay.
- Any feature, library, or architecture choice MUST be validated
  against these principles before implementation.
- Binary size is a **hard constraint** — features that bloat beyond
  budget MUST be rejected.
- Reliability over features — NEVER ship anything that risks config
  corruption or device bricking.
- All code changes MUST be verified on physical ESP8266 hardware
  before release.
- Constitution amendments require explicit approval from Sayed Elmahdy.
- Version follows semantic versioning:
  - MAJOR: Backward-incompatible governance/principle changes.
  - MINOR: New principle/section added or materially expanded.
  - PATCH: Clarifications, wording, or non-semantic refinements.

**Version**: 1.0.0 | **Ratified**: 2026-05-02 | **Last Amended**: 2026-05-02
