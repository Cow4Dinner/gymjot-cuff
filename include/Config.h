#pragma once

#include "MetadataTypes.h"

// Set to 0 for random passkey (check serial monitor), or set a fixed 6-digit code
#define BLE_FIXED_PASSKEY 123456  // Fixed passkey for production (no serial needed)

#define TEST_MODE_DEFAULT false
#define DEFAULT_FPS 12.0f
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

// Physical tag size (black square edge length) in meters
// IMPORTANT: Set this to the actual printed tag edge size.
// Common sizes are 0.050 (5 cm) or 0.160 (16 cm).
#define APRILTAG_TAG_SIZE_M 0.050f
#define APRILTAG_FX 615.0f
#define APRILTAG_FY 615.0f
#define APRILTAG_CX 160.0f
#define APRILTAG_CY 120.0f
#define APRILTAG_QUAD_DECIMATE 1.0f
#define APRILTAG_QUAD_SIGMA 0.0f
#define APRILTAG_REFINE_EDGES 1
#define APRILTAG_MIN_DECISION_MARGIN 12.0
#define APRILTAG_STABILITY_FRAMES 1

// Sharpening factor during decode; higher can help at distance but increases noise
#ifndef APRILTAG_DECODE_SHARPENING
#define APRILTAG_DECODE_SHARPENING 0.50f
#endif

// Optional distance calibration
// After pose estimation, distances are scaled and offset to improve accuracy.
// Measure at a known distance and adjust:
//   distance_cm_calibrated = distance_cm_raw * APRILTAG_DISTANCE_SCALE + APRILTAG_DISTANCE_OFFSET_CM
// Example: if raw = 123cm at actual 30.48cm, set scale ~= 0.247
#ifndef APRILTAG_DISTANCE_SCALE
#define APRILTAG_DISTANCE_SCALE 1.0f
#endif
#ifndef APRILTAG_DISTANCE_OFFSET_CM
#define APRILTAG_DISTANCE_OFFSET_CM 0.0f
#endif

// AprilTag family selection (compile-time)
// Options:
//  - APRILTAG_FAMILY_TAG36H10
//  - APRILTAG_FAMILY_TAG36H11
//  - APRILTAG_FAMILY_TAGCIRCLE49H12
//  - APRILTAG_FAMILY_TAGCUSTOM48H12
//  - APRILTAG_FAMILY_TAGSTANDARD41H12
#define APRILTAG_FAMILY_TAG36H10           1
#define APRILTAG_FAMILY_TAG36H11           2
#define APRILTAG_FAMILY_TAGCIRCLE49H12     3
#define APRILTAG_FAMILY_TAGCUSTOM48H12     4
#define APRILTAG_FAMILY_TAGSTANDARD41H12   5

#ifndef APRILTAG_FAMILY_SELECT
// Default to tagStandard41h12 to preserve current behavior.
#define APRILTAG_FAMILY_SELECT APRILTAG_FAMILY_TAGSTANDARD41H12
#endif

// Optionally enable compatibility detection for tag36h11 alongside the
// selected primary family. This helps if printed tags are 36h11.
#ifndef APRILTAG_ENABLE_COMPAT_36H11
#define APRILTAG_ENABLE_COMPAT_36H11 1
#endif

// AprilTag decoder error-correction limit.
// Warning: values >0 dramatically increase RAM use due to the quick-decode table.
// On ESP32 with limited heap, prefer 0 for large families like tagStandard41h12.
#ifndef APRILTAG_MAX_BITS_CORRECTED
#define APRILTAG_MAX_BITS_CORRECTED 0
#endif

// Allocate AprilTag quick-decode table in PSRAM (ESP32) when available.
// This is only consulted by the C library when APRILTAG_USE_PSRAM is defined
// at compile time for that translation unit. We also set this via build flags.
#ifndef APRILTAG_USE_PSRAM
#define APRILTAG_USE_PSRAM 1
#endif

#define SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_RX_UUID "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_TX_UUID "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_INFO_UUID "6E400004-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_SNAPSHOT_UUID "6E400005-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_OTA_UUID "6E400006-B5A3-F393-E0A9-E50E24DCCA9E"

#define MANUFACTURER_ID 0xFFFF

