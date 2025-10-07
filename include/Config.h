#pragma once

#include "MetadataTypes.h"

// Set to 0 for random passkey (check serial monitor), or set a fixed 6-digit code
#define BLE_FIXED_PASSKEY 123456  // Fixed passkey for production (no serial needed)

#define TEST_MODE_DEFAULT false
#define DEFAULT_FPS 8.0f
#define LOITER_FPS 0.3333f
#define APRILTAG_LOST_MS 10000

#define DEFAULT_MIN_REP_TRAVEL_CM 12.0f
#define DEFAULT_MAX_REP_IDLE_MS 5000

#define TEST_EXERCISE_ID 4242
#define TEST_EXERCISE_NAME "Demo Exercise"

inline gymjot::MetadataList defaultTestExerciseMetadata() {
    return gymjot::MetadataList{
        {"exercise", "Lat Pulldown"},
        {"muscleGroup", "Back"},
        {"intensity", "moderate"}
    };
}

#define APRILTAG_TAG_SIZE_M 0.055f
#define APRILTAG_FX 615.0f
#define APRILTAG_FY 615.0f
#define APRILTAG_CX 160.0f
#define APRILTAG_CY 120.0f
#define APRILTAG_QUAD_DECIMATE 1.5f
#define APRILTAG_QUAD_SIGMA 0.0f
#define APRILTAG_REFINE_EDGES 1
#define APRILTAG_MIN_DECISION_MARGIN 50.0
#define APRILTAG_STABILITY_FRAMES 3

#define SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_RX_UUID "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_TX_UUID "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_INFO_UUID "6E400004-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_SNAPSHOT_UUID "6E400005-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_OTA_UUID "6E400006-B5A3-F393-E0A9-E50E24DCCA9E"

#define MANUFACTURER_ID 0xFFFF

