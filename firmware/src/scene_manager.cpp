#include "scene_manager.h"
#include "config_manager.h"
#include "relay_controller.h"
#include <time.h>

void SceneManager::begin(ConfigManager& config, RelayController& relay) {
    _config = &config;
    _relay  = &relay;
}

bool SceneManager::createScene(const char* name, const JsonObject& states, JsonObjectConst schedule) {
    if (!_config || !name || name[0] == '\0') { return false; }

    JsonDocument& doc = _config->sceneConfigMut();
    if (!doc["scenes"].is<JsonArray>()) {
        doc["scenes"].to<JsonArray>();
    }
    JsonArray arr = doc["scenes"].as<JsonArray>();
    if (arr.size() >= 10) { return false; }
    if (_findScene(name) >= 0) { return false; }

    JsonObject scene  = arr.add<JsonObject>();
    scene["name"]     = name;
    JsonObject stObj  = scene["states"].to<JsonObject>();
    for (JsonPair kv : states) {
        stObj[kv.key()] = kv.value();
    }
    if (!schedule.isNull()) {
        JsonObject schObj        = scene["schedule"].to<JsonObject>();
        schObj["enabled"]        = schedule["enabled"]    | false;
        schObj["hour"]           = schedule["hour"]       | static_cast<uint8_t>(0);
        schObj["minute"]         = schedule["minute"]     | static_cast<uint8_t>(0);
        schObj["repeatMode"]     = schedule["repeatMode"] | "daily";
        schObj["dayMask"]        = schedule["dayMask"]    | static_cast<uint8_t>(0);
    }
    _config->saveScenes();
    return true;
}

bool SceneManager::activate(const char* name) {
    if (!_config || !_relay || !name) { return false; }
    int idx = _findScene(name);
    if (idx < 0) { return false; }

    JsonDocument& doc = _config->sceneConfigMut();
    JsonArray arr     = doc["scenes"].as<JsonArray>();
    JsonObject scene  = arr[idx].as<JsonObject>();
    JsonObject states = scene["states"].as<JsonObject>();

    _relay->startBatch();
    for (JsonPair kv : states) {
        int ch = atoi(kv.key().c_str());
        if (ch < 1) { continue; }
        const char* val = kv.value() | "off";
        bool on = (strcmp(val, "on") == 0);
        _relay->setState(static_cast<uint8_t>(ch), on);
    }
    _relay->endBatch();
    return true;
}

bool SceneManager::deleteScene(const char* name) {
    if (!_config || !name) { return false; }
    int idx = _findScene(name);
    if (idx < 0) { return false; }

    JsonDocument& doc = _config->sceneConfigMut();
    JsonArray arr     = doc["scenes"].as<JsonArray>();
    int last = static_cast<int>(arr.size()) - 1;
    if (idx != last) {
        arr[idx].set(arr[last]);
    }
    arr.remove(last);
    _config->saveScenes();
    return true;
}

void SceneManager::getScenes(JsonDocument& out) const {
    JsonArray dst = out["scenes"].to<JsonArray>();
    if (!_config) { return; }

    JsonDocument& doc = _config->sceneConfigMut();
    if (!doc["scenes"].is<JsonArray>()) { return; }
    JsonArray src = doc["scenes"].as<JsonArray>();

    for (JsonObject sc : src) {
        JsonObject dsc      = dst.add<JsonObject>();
        dsc["name"]         = sc["name"];
        JsonObject dstates  = dsc["states"].to<JsonObject>();
        if (sc["states"].is<JsonObject>()) {
            for (JsonPair kv : sc["states"].as<JsonObject>()) {
                dstates[kv.key()] = kv.value();
            }
        }
        if (sc["schedule"].is<JsonObjectConst>()) {
            JsonObject dsch          = dsc["schedule"].to<JsonObject>();
            JsonObjectConst ssch     = sc["schedule"].as<JsonObjectConst>();
            dsch["enabled"]          = ssch["enabled"]    | false;
            dsch["hour"]             = ssch["hour"]       | static_cast<uint8_t>(0);
            dsch["minute"]           = ssch["minute"]     | static_cast<uint8_t>(0);
            dsch["repeatMode"]       = ssch["repeatMode"] | "daily";
            dsch["dayMask"]          = ssch["dayMask"]    | static_cast<uint8_t>(0);
        }
    }
}

uint8_t SceneManager::count() const {
    if (!_config) { return 0; }
    JsonDocument& doc = _config->sceneConfigMut();
    if (!doc["scenes"].is<JsonArray>()) { return 0; }
    return static_cast<uint8_t>(doc["scenes"].as<JsonArray>().size());
}

int SceneManager::_findScene(const char* name) const {
    if (!_config) { return -1; }
    JsonDocument& doc = _config->sceneConfigMut();
    if (!doc["scenes"].is<JsonArray>()) { return -1; }
    JsonArray arr = doc["scenes"].as<JsonArray>();
    for (int i = 0; i < static_cast<int>(arr.size()); i++) {
        const char* n = arr[i]["name"] | "";
        if (strcasecmp(n, name) == 0) { return i; }
    }
    return -1;
}

void SceneManager::tick() {
    if (!_config || !_relay) return;
    const time_t t = time(nullptr);
    if (t < 1577836800L) return;  // NTP not synced

    const uint32_t minEpoch = (static_cast<uint32_t>(t) / 60u) * 60u;
    if (minEpoch == _lastScheduledMinEpoch) return;
    _lastScheduledMinEpoch = minEpoch;

    struct tm* ti = localtime(&t);
    const uint8_t curHour = static_cast<uint8_t>(ti->tm_hour);
    const uint8_t curMin  = static_cast<uint8_t>(ti->tm_min);
    const uint8_t curWday = static_cast<uint8_t>(ti->tm_wday);  // 0=Sun
    const uint8_t dayBit  = static_cast<uint8_t>(1u << curWday);

    JsonDocument& doc = _config->sceneConfigMut();
    if (!doc["scenes"].is<JsonArray>()) return;
    JsonArray arr = doc["scenes"].as<JsonArray>();

    bool needSave = false;
    for (JsonObject sc : arr) {
        if (!(sc["schedule"]["enabled"] | false)) continue;
        const uint8_t sHour = sc["schedule"]["hour"]   | static_cast<uint8_t>(255);
        const uint8_t sMin  = sc["schedule"]["minute"] | static_cast<uint8_t>(255);
        if (sHour != curHour || sMin != curMin) continue;

        const char* repeatMode = sc["schedule"]["repeatMode"] | "daily";
        const uint8_t dayMask  = sc["schedule"]["dayMask"]    | static_cast<uint8_t>(0);

        bool shouldFire = false;
        if      (strcmp(repeatMode, "once")     == 0) { shouldFire = true; }
        else if (strcmp(repeatMode, "daily")    == 0) { shouldFire = true; }
        else if (strcmp(repeatMode, "weekdays") == 0) { shouldFire = (curWday >= 1 && curWday <= 5); }
        else if (strcmp(repeatMode, "weekend")  == 0) { shouldFire = (curWday == 0 || curWday == 6); }
        else if (strcmp(repeatMode, "custom")   == 0) { shouldFire = (dayMask & dayBit) != 0; }

        if (shouldFire) {
            const char* name = sc["name"] | "";
            Serial.printf("[SceneManager] Schedule firing: %s\n", name);
            activate(name);
            if (strcmp(repeatMode, "once") == 0) {
                sc["schedule"]["enabled"] = false;
                needSave = true;
            }
        }
    }
    if (needSave) _config->saveScenes();
}
