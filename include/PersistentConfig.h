#pragma once

#include <cstdint>

namespace gymjot {

struct PersistentSettings {
    bool hasTargetFps = false;
    float targetFps = 0.0f;
    bool hasLoiterFps = false;
    float loiterFps = 0.0f;
    bool hasMinTravelCm = false;
    float minTravelCm = 0.0f;
    bool hasMaxRepIdleMs = false;
    uint32_t maxRepIdleMs = 0;
};

bool loadPersistentSettings(PersistentSettings& out);
void storeTargetFps(float value);
void storeLoiterFps(float value);
void storeMinTravelCm(float value);
void storeMaxRepIdleMs(uint32_t value);
void clearPersistentSettings();

}  // namespace gymjot
