/*
 * timer_engine.cpp — Countdown timer management with LittleFS persistence.
 *
 * See timer_engine.h for the full design contract.
 *
 * Persistence schema (timers.json):
 * {
 *   "nextId": <uint16>,
 *   "timers": [
 *     {
 *       "id": 1,
 *       "channel": 1,
 *       "type": "countdown",
 *       "targetState": "off",
 *       "enabled": true,
 *       "duration": 1800000,
 *       "startedAt": 1700000000
 *     },
 *     …
 *   ],
 *   "crc": <uint32>    ← managed by ConfigManager::write()
 * }
 *
 * startedAt is epoch seconds; remaining = duration - (now - startedAt)*1000.
 * If NTP is unavailable, startedAt is millis()/1000 (monotonic fallback).
 * In that case a cold boot resets millis() to 0, so we conservatively treat
 * remaining = duration (full reset) rather than fire early.
 *
 * Target: ESP8266 (ESP-12F / NodeMCU), Arduino Core 3.x, C++17
 */

#include "timer_engine.h"
#include "config_manager.h"
#include "relay_controller.h"

#include <Arduino.h>
#include <ctime>

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

TimerEngine::TimerEngine()
    : _count(0)
    , _config(nullptr)
    , _relay(nullptr)
    , _lastBroadcastMs(0)
    , _lastScheduledMinEpoch(0)
{
    // Zero the entire fixed array so valgrind / ASAN would catch uninitialised
    // reads if this were ever run in a hosted test.
    memset(_timers, 0, sizeof(_timers));
}

// ─────────────────────────────────────────────────────────────────────────────
// begin()
// ─────────────────────────────────────────────────────────────────────────────

void TimerEngine::begin(ConfigManager& config, RelayController& relay) {
    _config = &config;
    _relay  = &relay;
    _count  = 0;

    const uint32_t nowEpoch = _epochNow();
    const uint32_t nowMs    = millis();

    const JsonDocument& timerDoc = _config->timerConfig();
    JsonArrayConst arr = timerDoc["timers"].as<JsonArrayConst>();

    for (JsonObjectConst obj : arr) {
        const char* type       = obj["type"] | "countdown";
        const bool  isCountdown = (strcmp(type, "countdown") == 0);
        const bool  isScheduled = (strcmp(type, "scheduled") == 0);

        if (!isCountdown && !isScheduled) continue;

        if (_count >= MAX_TIMERS) {
            Serial.println(F("[Timer] begin: MAX_TIMERS reached, skipping rest"));
            break;
        }

        const uint16_t id          = obj["id"]          | 0;
        const uint8_t  channel     = obj["channel"]     | 0;
        const char*    targetState = obj["targetState"] | "off";
        const bool     enabled     = obj["enabled"]     | true;

        if (id == 0 || channel == 0) {
            Serial.printf_P(PSTR("[Timer] begin: skipping malformed entry id=%u\n"), id);
            continue;
        }

        if (isScheduled) {
            TimerEntry& entry = _timers[_count++];
            entry.id      = id;
            entry.channel = channel;
            strlcpy(entry.targetState, targetState, sizeof(entry.targetState));
            entry.enabled     = enabled;
            strlcpy(entry.type, "scheduled", sizeof(entry.type));
            entry.hour        = obj["hour"]       | 0;
            entry.minute      = obj["minute"]     | 0;
            strlcpy(entry.repeatMode,
                    obj["repeatMode"] | "daily",
                    sizeof(entry.repeatMode));
            entry.dayMask     = obj["dayMask"]    | 0;
            entry.duration    = 0;
            entry.startedAt   = 0;
            entry.remainingMs = 0;
            entry.lastMillis  = 0;
            Serial.printf_P(PSTR("[Timer] begin: restored scheduled id=%u ch=%u at %02u:%02u\n"),
                            id, channel, entry.hour, entry.minute);
            continue;
        }

        // Countdown timer — compute remaining with elapsed-time correction.
        const uint32_t duration  = obj["duration"]   | 0;
        const uint32_t startedAt = obj["startedAt"]  | 0;

        if (duration == 0) {
            Serial.printf_P(PSTR("[Timer] begin: skipping malformed entry id=%u\n"), id);
            continue;
        }

        int64_t elapsedMs = 0;
        if (startedAt > 0 && nowEpoch >= startedAt) {
            elapsedMs = static_cast<int64_t>(nowEpoch - startedAt) * 1000LL;
        }
        // If startedAt == 0 (NTP never synced) we cannot safely compute elapsed
        // time after a cold reboot because millis() resets.  Treat remaining as
        // the full duration so the user gets a fresh countdown.

        int64_t remainingMs = static_cast<int64_t>(duration) - elapsedMs;

        if (remainingMs <= 0) {
            Serial.printf_P(PSTR("[Timer] begin: timer %u expired offline, firing\n"), id);
            TimerEntry tmp{};
            tmp.id      = id;
            tmp.channel = channel;
            strlcpy(tmp.targetState, targetState, sizeof(tmp.targetState));
            strlcpy(tmp.type, "countdown", sizeof(tmp.type));
            _fire(tmp);
            continue;
        }

        TimerEntry& entry    = _timers[_count++];
        entry.id             = id;
        entry.channel        = channel;
        strlcpy(entry.targetState, targetState, sizeof(entry.targetState));
        entry.enabled        = enabled;
        strlcpy(entry.type, "countdown", sizeof(entry.type));
        entry.duration       = duration;
        entry.startedAt      = startedAt;
        entry.hour           = 0;
        entry.minute         = 0;
        entry.dayMask        = 0;
        entry.remainingMs    = static_cast<uint32_t>(remainingMs);
        entry.lastMillis     = nowMs;

        Serial.printf_P(PSTR("[Timer] begin: restored id=%u ch=%u remaining=%lums\n"),
                        id, channel, static_cast<unsigned long>(remainingMs));
    }

    // Re-persist if any timers fired or were skipped.
    uint8_t diskCount = 0;
    for (JsonObjectConst obj : arr) {
        const char* t = obj["type"] | "countdown";
        if (strcmp(t, "countdown") == 0 || strcmp(t, "scheduled") == 0) ++diskCount;
    }

    if (_count != diskCount) {
        _persist();
    }

    Serial.printf_P(PSTR("[Timer] begin: %u timer(s) active\n"), _count);
    _lastBroadcastMs = millis();
}

// ─────────────────────────────────────────────────────────────────────────────
// tick()
// ─────────────────────────────────────────────────────────────────────────────

void TimerEngine::tick() {
    if (_count == 0) return;

    const uint32_t now = millis();

    // Advance remaining time for countdown timers; fire expired ones.
    // Iterate backwards so _removeAt() does not disturb unvisited indices.
    for (int8_t i = static_cast<int8_t>(_count) - 1; i >= 0; --i) {
        TimerEntry& entry = _timers[static_cast<uint8_t>(i)];

        if (!entry.enabled) continue;
        if (strcmp(entry.type, "countdown") != 0) continue;

        uint32_t delta = now - entry.lastMillis;
        entry.lastMillis = now;

        if (delta >= entry.remainingMs) {
            entry.remainingMs = 0;
            Serial.printf_P(PSTR("[Timer] tick: timer %u expired (ch=%u → %s)\n"),
                            entry.id, entry.channel, entry.targetState);
            _fire(entry);
            _removeAt(static_cast<uint8_t>(i));
            _persist();
        } else {
            entry.remainingMs -= delta;
        }
    }

    // Check scheduled timers at most once per minute when NTP is available.
    {
        const time_t t = time(nullptr);
        if (t >= 1577836800L) {
            const uint32_t minEpoch = static_cast<uint32_t>(t / 60) * 60;
            if (minEpoch != _lastScheduledMinEpoch) {
                _lastScheduledMinEpoch = minEpoch;
                _tickScheduled();
            }
        }
    }

    // Broadcast remaining times approximately every 1000 ms.
    if ((now - _lastBroadcastMs) >= 1000UL) {
        _lastBroadcastMs = now;
        if (_onTick) {
            _onTick();
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// createCountdown()
// ─────────────────────────────────────────────────────────────────────────────

uint16_t TimerEngine::createCountdown(uint8_t channel,
                                      uint32_t durationMs,
                                      const char* targetState)
{
    if (_count >= MAX_TIMERS) {
        Serial.println(F("[Timer] createCountdown: limit reached"));
        return 0;
    }

    if (channel == 0 || channel > MAX_CHANNELS) {
        Serial.printf_P(PSTR("[Timer] createCountdown: invalid channel %u\n"), channel);
        return 0;
    }

    if (durationMs == 0) {
        Serial.println(F("[Timer] createCountdown: zero duration"));
        return 0;
    }

    if (!_validateTargetState(targetState)) {
        Serial.printf_P(PSTR("[Timer] createCountdown: invalid targetState '%s'\n"),
                        targetState);
        return 0;
    }

    // Allocate the next ID from the persisted counter.
    JsonDocument& timerDoc = _config->timerConfigMut();
    uint16_t nextId = timerDoc["nextId"] | 1;
    if (nextId == 0) nextId = 1;  // Guard against corrupt value.

    const uint32_t nowMs    = millis();
    const uint32_t nowEpoch = _epochNow();

    TimerEntry& entry    = _timers[_count++];
    entry.id             = nextId;
    entry.channel        = channel;
    strlcpy(entry.targetState, targetState, sizeof(entry.targetState));
    entry.enabled        = true;
    strlcpy(entry.type, "countdown", sizeof(entry.type));
    entry.duration       = durationMs;
    entry.startedAt      = nowEpoch;
    entry.hour           = 0;
    entry.minute         = 0;
    entry.dayMask        = 0;
    entry.remainingMs    = durationMs;
    entry.lastMillis     = nowMs;

    // Advance nextId and persist immediately.
    timerDoc["nextId"] = static_cast<uint16_t>(nextId + 1);
    _persist();

    Serial.printf_P(PSTR("[Timer] created id=%u ch=%u dur=%lums target=%s\n"),
                    nextId, channel,
                    static_cast<unsigned long>(durationMs),
                    targetState);

    return nextId;
}

// ─────────────────────────────────────────────────────────────────────────────
// createScheduled()  — T038
// ─────────────────────────────────────────────────────────────────────────────

uint16_t TimerEngine::createScheduled(uint8_t channel,
                                      const char* targetState,
                                      uint8_t hour,
                                      uint8_t minute,
                                      const char* repeatMode,
                                      uint8_t dayMask)
{
    if (_count >= MAX_TIMERS) {
        Serial.println(F("[Timer] createScheduled: limit reached"));
        return 0;
    }
    if (channel == 0 || channel > MAX_CHANNELS) {
        Serial.printf_P(PSTR("[Timer] createScheduled: invalid channel %u\n"), channel);
        return 0;
    }
    if (!_validateTargetState(targetState)) {
        Serial.printf_P(PSTR("[Timer] createScheduled: invalid targetState '%s'\n"), targetState);
        return 0;
    }
    if (hour > 23 || minute > 59) {
        Serial.println(F("[Timer] createScheduled: hour/minute out of range"));
        return 0;
    }
    if (!_validateRepeatMode(repeatMode)) {
        Serial.printf_P(PSTR("[Timer] createScheduled: invalid repeatMode '%s'\n"), repeatMode);
        return 0;
    }

    JsonDocument& timerDoc = _config->timerConfigMut();
    uint16_t nextId = timerDoc["nextId"] | 1;
    if (nextId == 0) nextId = 1;

    TimerEntry& entry = _timers[_count++];
    entry.id      = nextId;
    entry.channel = channel;
    strlcpy(entry.targetState, targetState, sizeof(entry.targetState));
    entry.enabled     = true;
    strlcpy(entry.type, "scheduled", sizeof(entry.type));
    entry.hour        = hour;
    entry.minute      = minute;
    strlcpy(entry.repeatMode, repeatMode, sizeof(entry.repeatMode));
    entry.dayMask     = dayMask;
    entry.duration    = 0;
    entry.startedAt   = 0;
    entry.remainingMs = 0;
    entry.lastMillis  = 0;

    timerDoc["nextId"] = static_cast<uint16_t>(nextId + 1);
    _persist();

    Serial.printf_P(PSTR("[Timer] scheduled: created id=%u ch=%u at %02u:%02u repeat=%s mask=0x%02x\n"),
                    nextId, channel, hour, minute, repeatMode, dayMask);
    return nextId;
}

// ─────────────────────────────────────────────────────────────────────────────
// cancel()
// ─────────────────────────────────────────────────────────────────────────────

bool TimerEngine::cancel(uint16_t timerId) {
    for (uint8_t i = 0; i < _count; ++i) {
        if (_timers[i].id == timerId) {
            Serial.printf_P(PSTR("[Timer] cancel: removed id=%u\n"), timerId);
            _removeAt(i);
            _persist();
            return true;
        }
    }
    Serial.printf_P(PSTR("[Timer] cancel: id=%u not found\n"), timerId);
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// getTimers()
// ─────────────────────────────────────────────────────────────────────────────

uint8_t TimerEngine::getTimers(JsonDocument& outDoc) const {
    outDoc.clear();
    JsonArray arr = outDoc["timers"].to<JsonArray>();

    const uint32_t now = millis();

    for (uint8_t i = 0; i < _count; ++i) {
        const TimerEntry& entry = _timers[i];
        const bool isScheduled = (strcmp(entry.type, "scheduled") == 0);

        JsonObject obj = arr.add<JsonObject>();
        obj["id"]          = entry.id;
        obj["channel"]     = entry.channel;
        obj["type"]        = entry.type[0] ? entry.type : "countdown";
        obj["targetState"] = entry.targetState;
        obj["enabled"]     = entry.enabled;

        if (isScheduled) {
            obj["hour"]       = entry.hour;
            obj["minute"]     = entry.minute;
            obj["repeatMode"] = entry.repeatMode;
            obj["dayMask"]    = entry.dayMask;
            obj["remaining"]  = 0;
            char tBuf[16];
            int h = entry.hour % 12;
            if (h == 0) h = 12;
            snprintf(tBuf, sizeof(tBuf), "%02d:%02d %s", h, entry.minute, entry.hour >= 12 ? "PM" : "AM");
            obj["time"]       = tBuf;
        } else {
            uint32_t delta     = now - entry.lastMillis;
            uint32_t remaining = (delta >= entry.remainingMs) ? 0 : (entry.remainingMs - delta);
            obj["duration"]    = entry.duration;
            obj["remaining"]   = remaining;
        }
    }

    return _count;
}

// ─────────────────────────────────────────────────────────────────────────────
// getRemaining()
// ─────────────────────────────────────────────────────────────────────────────

uint32_t TimerEngine::getRemaining(uint16_t timerId) const {
    const TimerEntry* entry = _findById(timerId);
    if (entry == nullptr) return 0;

    const uint32_t now   = millis();
    const uint32_t delta = now - entry->lastMillis;
    if (delta >= entry->remainingMs) return 0;
    return entry->remainingMs - delta;
}

// ─────────────────────────────────────────────────────────────────────────────
// Callback setters
// ─────────────────────────────────────────────────────────────────────────────

void TimerEngine::setOnExpiry(std::function<void(uint8_t, const char*)> cb) {
    _onExpiry = cb;
}

void TimerEngine::setOnTick(std::function<void()> cb) {
    _onTick = cb;
}

// ─────────────────────────────────────────────────────────────────────────────
// Private helpers
// ─────────────────────────────────────────────────────────────────────────────

TimerEntry* TimerEngine::_findById(uint16_t id) {
    for (uint8_t i = 0; i < _count; ++i) {
        if (_timers[i].id == id) return &_timers[i];
    }
    return nullptr;
}

const TimerEntry* TimerEngine::_findById(uint16_t id) const {
    for (uint8_t i = 0; i < _count; ++i) {
        if (_timers[i].id == id) return &_timers[i];
    }
    return nullptr;
}

void TimerEngine::_removeAt(uint8_t index) {
    if (index >= _count) return;
    // Swap with the last slot and shrink.
    if (index < _count - 1) {
        _timers[index] = _timers[_count - 1];
    }
    memset(&_timers[_count - 1], 0, sizeof(TimerEntry));
    --_count;
}

void TimerEngine::_fire(const TimerEntry& entry) {
    if (_relay != nullptr) {
        const char* ts = entry.targetState;
        if (strcmp(ts, "on") == 0) {
            _relay->setState(entry.channel, true);
        } else if (strcmp(ts, "off") == 0) {
            _relay->setState(entry.channel, false);
        } else if (strcmp(ts, "toggle") == 0) {
            _relay->toggle(entry.channel);
        }
    }

    if (_onExpiry) {
        _onExpiry(entry.channel, entry.targetState);
    }
}

void TimerEngine::_persist() {
    if (_config == nullptr) return;

    JsonDocument& timerDoc = _config->timerConfigMut();

    // Preserve nextId; rebuild the timers array from scratch.
    uint16_t nextId = timerDoc["nextId"] | 1;

    timerDoc.clear();
    timerDoc["nextId"] = nextId;
    JsonArray arr = timerDoc["timers"].to<JsonArray>();

    for (uint8_t i = 0; i < _count; ++i) {
        const TimerEntry& entry = _timers[i];
        const bool isScheduled = (strcmp(entry.type, "scheduled") == 0);

        JsonObject obj = arr.add<JsonObject>();
        obj["id"]          = entry.id;
        obj["channel"]     = entry.channel;
        obj["type"]        = entry.type[0] ? entry.type : "countdown";
        obj["targetState"] = entry.targetState;
        obj["enabled"]     = entry.enabled;

        if (isScheduled) {
            obj["hour"]       = entry.hour;
            obj["minute"]     = entry.minute;
            obj["repeatMode"] = entry.repeatMode;
            obj["dayMask"]    = entry.dayMask;
        } else {
            obj["duration"]  = entry.duration;
            obj["startedAt"] = entry.startedAt;
        }
        // remainingMs / lastMillis are runtime fields — not persisted.
    }

    _config->saveTimers();
}

/*static*/
uint32_t TimerEngine::_epochNow() {
    // time() returns (time_t)-1 when the clock is not set.
    // A valid NTP-synced time is always >= Jan 1 2020 (epoch 1577836800).
    const time_t t = time(nullptr);
    if (t >= 1577836800L) {
        return static_cast<uint32_t>(t);
    }
    // Fallback: monotonic seconds since boot.  Not wall-clock, but safe for
    // in-session timers.  Cross-reboot accuracy requires NTP.
    return static_cast<uint32_t>(millis() / 1000UL);
}

/*static*/
bool TimerEngine::_validateTargetState(const char* s) {
    if (s == nullptr) return false;
    return (strcmp(s, "on")     == 0 ||
            strcmp(s, "off")    == 0 ||
            strcmp(s, "toggle") == 0);
}

/*static*/
bool TimerEngine::_validateRepeatMode(const char* s) {
    if (s == nullptr) return false;
    return (strcmp(s, "once")   == 0 ||
            strcmp(s, "daily")  == 0 ||
            strcmp(s, "weekly") == 0 ||
            strcmp(s, "custom") == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// _tickScheduled()  — T038
//
// Called from tick() at most once per minute when NTP clock is valid.
// Iterates backwards so _removeAt() does not disturb unvisited indices.
// ─────────────────────────────────────────────────────────────────────────────

void TimerEngine::_tickScheduled() {
    const time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    if (!t) return;

    const uint8_t curHour   = static_cast<uint8_t>(t->tm_hour);
    const uint8_t curMin    = static_cast<uint8_t>(t->tm_min);
    const uint8_t dayBit    = static_cast<uint8_t>(1u << t->tm_wday);  // 0=Sun

    for (int8_t i = static_cast<int8_t>(_count) - 1; i >= 0; --i) {
        TimerEntry& entry = _timers[static_cast<uint8_t>(i)];
        if (!entry.enabled) continue;
        if (strcmp(entry.type, "scheduled") != 0) continue;
        if (entry.hour != curHour || entry.minute != curMin) continue;
        // dayMask == 0 means every day; otherwise check the current weekday bit.
        if (entry.dayMask != 0 && !(entry.dayMask & dayBit)) continue;

        Serial.printf_P(PSTR("[Timer] scheduled: firing id=%u ch=%u → %s\n"),
                        entry.id, entry.channel, entry.targetState);
        _fire(entry);

        if (strcmp(entry.repeatMode, "once") == 0) {
            _removeAt(static_cast<uint8_t>(i));
            _persist();
        }
        // "daily", "weekly", "custom" — keep for next occurrence.
    }
}
