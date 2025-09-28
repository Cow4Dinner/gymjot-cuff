#include "PersistentConfig.h"

#ifdef ARDUINO
#include <Preferences.h>

namespace gymjot {
namespace {
constexpr const char* kNamespace = "cuffcfg";
constexpr const char* kKeyTarget = "target";
constexpr const char* kKeyLoiter = "loiter";
constexpr const char* kKeyMinTravel = "mintr";
constexpr const char* kKeyMaxIdle = "maxidle";

bool readFloat(Preferences& prefs, const char* key, float& out) {
    if (!prefs.isKey(key)) {
        return false;
    }
    out = prefs.getFloat(key, 0.0f);
    return true;
}

bool readUInt(Preferences& prefs, const char* key, uint32_t& out) {
    if (!prefs.isKey(key)) {
        return false;
    }
    out = prefs.getUInt(key, 0U);
    return true;
}

}  // namespace

bool loadPersistentSettings(PersistentSettings& out) {
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) {
        return false;
    }
    bool any = false;
    if (readFloat(prefs, kKeyTarget, out.targetFps)) {
        out.hasTargetFps = true;
        any = true;
    }
    if (readFloat(prefs, kKeyLoiter, out.loiterFps)) {
        out.hasLoiterFps = true;
        any = true;
    }
    if (readFloat(prefs, kKeyMinTravel, out.minTravelCm)) {
        out.hasMinTravelCm = true;
        any = true;
    }
    if (readUInt(prefs, kKeyMaxIdle, out.maxRepIdleMs)) {
        out.hasMaxRepIdleMs = true;
        any = true;
    }
    prefs.end();
    return any;
}

void storeTargetFps(float value) {
    Preferences prefs;
    prefs.begin(kNamespace, false);
    prefs.putFloat(kKeyTarget, value);
    prefs.end();
}

void storeLoiterFps(float value) {
    Preferences prefs;
    prefs.begin(kNamespace, false);
    prefs.putFloat(kKeyLoiter, value);
    prefs.end();
}

void storeMinTravelCm(float value) {
    Preferences prefs;
    prefs.begin(kNamespace, false);
    prefs.putFloat(kKeyMinTravel, value);
    prefs.end();
}

void storeMaxRepIdleMs(uint32_t value) {
    Preferences prefs;
    prefs.begin(kNamespace, false);
    prefs.putUInt(kKeyMaxIdle, value);
    prefs.end();
}

void clearPersistentSettings() {
    Preferences prefs;
    prefs.begin(kNamespace, false);
    prefs.clear();
    prefs.end();
}

}  // namespace gymjot
#else

namespace gymjot {
namespace {
PersistentSettings g_settings;
}

bool loadPersistentSettings(PersistentSettings& out) {
    out = g_settings;
    return g_settings.hasTargetFps || g_settings.hasLoiterFps || g_settings.hasMinTravelCm || g_settings.hasMaxRepIdleMs;
}

void storeTargetFps(float value) {
    g_settings.targetFps = value;
    g_settings.hasTargetFps = true;
}

void storeLoiterFps(float value) {
    g_settings.loiterFps = value;
    g_settings.hasLoiterFps = true;
}

void storeMinTravelCm(float value) {
    g_settings.minTravelCm = value;
    g_settings.hasMinTravelCm = true;
}

void storeMaxRepIdleMs(uint32_t value) {
    g_settings.maxRepIdleMs = value;
    g_settings.hasMaxRepIdleMs = true;
}

void clearPersistentSettings() {
    g_settings = PersistentSettings{};
}

}  // namespace gymjot

#endif  // ARDUINO
