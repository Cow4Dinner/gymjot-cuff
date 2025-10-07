#include <Arduino.h>
#include <NimBLEDevice.h>
#include <pb_decode.h>
#include <pb_encode.h>

#include "proto/cuff.pb.h"
#include <array>
#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <cmath>
#include <cstdio>

extern "C" {
#include "apriltag.h"
#include "tagCircle49h12.h"
#include "apriltag_pose.h"
#include "common/image_u8.h"
#include "common/matd.h"
}

#include "esp_camera.h"
#include "Config.h"
#include "CuffController.h"
#include "DeviceIdentity.h"
#include "PersistentConfig.h"
#include "system/Diagnostics.h"

#include <esp_sleep.h>
#include <esp_task_wdt.h>

// ESP32-CAM (AI Thinker) pin mapping
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22
using gymjot::AprilTagDetection;
using gymjot::ControllerConfig;
using gymjot::CuffController;
using gymjot::ExercisePayload;

static NimBLEServer* g_server = nullptr;
static NimBLECharacteristic* g_tx = nullptr;
static NimBLECharacteristic* g_snapshotChar = nullptr;
static NimBLECharacteristic* g_infoChar = nullptr;
static NimBLECharacteristic* g_commandChar = nullptr;
static NimBLECharacteristic* g_otaChar = nullptr;
static std::unique_ptr<CuffController> g_controller;
static bool g_cameraReady = false;
static apriltag_family_t* g_tagFamily = nullptr;
static apriltag_detector_t* g_tagDetector = nullptr;
static bool g_otaInProgress = false;
static uint32_t g_otaTotalBytes = 0;
static uint32_t g_otaReceivedBytes = 0;
static const gymjot::DeviceIdentity* g_identity = nullptr;
static constexpr const char* kFirmwareVersion = "0.1.0";
static bool g_clientConnected = false;
static uint64_t g_lastConnectionTime = 0;
static int8_t g_lastRssi = 0;
static bool g_connectionOptimized = false;
static uint64_t g_lastAprilTagDetectionMs = 0;
static uint32_t g_lastAprilTagId = 0;
static float g_lastAprilTagDistanceCm = 0.0f;
static double g_lastAprilTagMargin = 0.0;

static camera_config_t g_grayscaleCameraConfig = {};
static camera_config_t g_photoCameraConfig = {};
static bool g_cameraConfigInitialized = false;

struct PendingPhotoRequest {
    bool pending;
    bool high_resolution;
    uint32_t session_id;
};

static PendingPhotoRequest g_pendingPhotoRequest{false, false, 0};
static bool g_photoCaptureInProgress = false;
static uint32_t g_photoSessionCounter = 0;

// Video streaming state
struct VideoStreamState {
    bool active;
    bool apriltagEnabled;
    bool motionEnabled;
    float fps;
    uint32_t sessionId;
    uint32_t frameNumber;
    uint64_t lastFrameMs;
    // Motion detection
    uint8_t* previousFrame;
    size_t previousFrameSize;
    uint32_t frameWidth;
    uint32_t frameHeight;
};
static VideoStreamState g_videoState = {false, false, false, 5.0f, 0, 0, 0, nullptr, 0, 0, 0};
static uint32_t g_videoSessionCounter = 0;

// Queue for deferred status notifications (avoid blocking GATT callbacks)
struct DeferredStatus {
    bool pending;
    char label[32];
    uint64_t timestamp;
};
static DeferredStatus g_deferredStatus{false, "", 0};
static constexpr size_t kPhotoChunkPayloadBytes = 160;
// Reduced from VGA to QVGA for high-res to prevent connection timeouts
// VGA was causing 20-40KB transfers that took too long
static constexpr framesize_t kPhotoFrameSizeHigh = FRAMESIZE_QVGA;  // 320x240 instead of 640x480
static constexpr framesize_t kPhotoFrameSizeLow = FRAMESIZE_QQVGA;  // 160x120
static constexpr int kPhotoQualityHigh = 15;  // Increased from 12 (lower number = higher quality/size)
static constexpr int kPhotoQualityLow = 25;   // Increased from 20
static constexpr const char* kPhotoMimeType = "image/jpeg";


static constexpr uint32_t kAutoResetGracePeriodMs = 3000;

static gymjot::system::HeapMonitor g_heapMonitor({
#ifdef ENABLE_HEAP_SERIAL_LOGGING
    true,
#else
    false,
#endif
    60 * 1024,
    400,
    5000,
    8 * 1024
});

static gymjot::system::ResetScheduler g_resetScheduler(
    kAutoResetGracePeriodMs,
    [](const char* reason) {
        esp_task_wdt_reset();
        delay(100);
        ESP.restart();
    });

static uint32_t g_aprilTagDetectionCount = 0;
static constexpr uint32_t kDetectionsBeforeAutoReset = 600;
static constexpr uint64_t kNoDetectionLogIntervalMs = 15000;
static constexpr uint64_t kWrongFamilyLogIntervalMs = 5000;

// Connection telemetry and instrumentation
static uint64_t g_connectTimestamp = 0;
static uint64_t g_mtuNegotiatedTimestamp = 0;
static uint64_t g_connParamsUpdatedTimestamp = 0;
static uint64_t g_firstAttRequestTimestamp = 0;
static uint8_t g_lastDisconnectReason = 0;
static uint16_t g_currentMtu = 23;  // Default BLE MTU
static uint16_t g_currentConnInterval = 0;  // in 1.25ms units
static uint16_t g_currentConnLatency = 0;
static uint16_t g_currentSupervisionTimeout = 0;  // in 10ms units

static constexpr size_t kProtoBufferSize = 512;
static constexpr size_t kLengthPrefixBytes = 2;

static com_gymjot_cuff_DeviceMode toProtoMode(gymjot::DeviceMode mode) {
    switch (mode) {
        case gymjot::DeviceMode::Idle:
            return com_gymjot_cuff_DeviceMode_DEVICE_MODE_IDLE;
        case gymjot::DeviceMode::AwaitingExercise:
            return com_gymjot_cuff_DeviceMode_DEVICE_MODE_AWAITING_EXERCISE;
        case gymjot::DeviceMode::Scanning:
            return com_gymjot_cuff_DeviceMode_DEVICE_MODE_SCANNING;
        case gymjot::DeviceMode::Loiter:
            return com_gymjot_cuff_DeviceMode_DEVICE_MODE_LOITER;
    }
    return com_gymjot_cuff_DeviceMode_DEVICE_MODE_IDLE;
}

static gymjot::MetadataList metadataFromProto(const com_gymjot_cuff_ExerciseMetadata& metadata) {
    gymjot::MetadataList result;
    result.reserve(metadata.entries_count);
    for (pb_size_t i = 0; i < metadata.entries_count; ++i) {
        const auto& entry = metadata.entries[i];
        result.push_back({entry.key, entry.value});
    }
    return result;
}

static bool encodeWithLength(const pb_msgdesc_t* fields, const void* src, std::array<uint8_t, kLengthPrefixBytes + kProtoBufferSize>& buffer, size_t& totalLen) {
    pb_ostream_t stream = pb_ostream_from_buffer(buffer.data() + kLengthPrefixBytes, kProtoBufferSize);
    if (!pb_encode(&stream, fields, src)) {
        Serial.print("encode error: ");
        Serial.println(PB_GET_ERROR(&stream));
        return false;
    }
    const size_t payloadLen = stream.bytes_written;
    if (payloadLen > 0xFFFF) {
        Serial.println("encode error: payload too large");
        return false;
    }
    buffer[0] = static_cast<uint8_t>(payloadLen & 0xFF);
    buffer[1] = static_cast<uint8_t>((payloadLen >> 8) & 0xFF);
    totalLen = payloadLen + kLengthPrefixBytes;
    return true;
}

static void updateSnapshotCharacteristic(uint64_t nowMs);
static void sendSnapshotEvent(uint64_t nowMs);
static void sendOtaStatus(com_gymjot_cuff_OtaPhase phase, const char* message, bool success, uint32_t transferred = 0, uint32_t total = 0);
static void sendPowerEvent(const char* state, uint64_t nowMs);
static float computeDetectionDistance(const apriltag_detection_t* det);
static std::string buildInfoString() {
    if (!g_identity) {
        g_identity = &gymjot::deviceIdentity();
    }
    char idBuffer[21] = {0};
    std::snprintf(idBuffer, sizeof(idBuffer), "%016llX", static_cast<unsigned long long>(g_identity->deviceId));
    std::string info = "name=";
    info += g_identity->name;
    info.push_back('\n');
    info += "id=0x";
    info += idBuffer;
    info.push_back('\n');
    info += "fw=";
    info += kFirmwareVersion;
    info.push_back('\n');
    info += "ota=";
    info += g_otaInProgress ? "true" : "false";
    return info;
}
static bool sendEvent(const com_gymjot_cuff_DeviceEvent& event);
static void processCommand(const uint8_t* data, size_t len);
static void sendStatusLabel(const char* label, uint64_t nowMs);
static void sendPhotoMetaEvent(uint32_t sessionId, uint32_t totalBytes, uint32_t width, uint32_t height, const char* mimeType, uint64_t nowMs);
static bool sendPhotoChunkEvent(uint32_t sessionId, uint32_t offset, const uint8_t* data, size_t length, bool finalChunk, uint64_t nowMs);
static void handlePendingPhotoRequest(uint64_t nowMs);
static bool captureAndSendPhoto(uint32_t sessionId, bool highResolution, uint64_t requestTimeMs);
static bool switchToPhotoCamera(framesize_t frameSize, int quality);
static bool restorePrimaryCamera();
static uint32_t nextPhotoSessionId();

static void logPacket(size_t len) {
    Serial.print("-> [");
    Serial.print(len);
    Serial.println(" bytes]");
}

static const char* protoModeLabel(com_gymjot_cuff_DeviceMode mode) {
    switch (mode) {
        case com_gymjot_cuff_DeviceMode_DEVICE_MODE_IDLE: return "Idle";
        case com_gymjot_cuff_DeviceMode_DEVICE_MODE_AWAITING_EXERCISE: return "AwaitingExercise";
        case com_gymjot_cuff_DeviceMode_DEVICE_MODE_SCANNING: return "Scanning";
        case com_gymjot_cuff_DeviceMode_DEVICE_MODE_LOITER: return "Loiter";
        default: return "Unknown";
    }
}

static const char* boolLabel(bool value) {
    return value ? "true" : "false";
}

template <size_t N>
static void logCStringField(const char* label, const char (&buffer)[N]) {
    size_t len = 0;
    while (len < N && buffer[len] != '\0') {
        ++len;
    }
    if (len == 0) {
        return;
    }
    Serial.print(label);
    for (size_t i = 0; i < len; ++i) {
        char c = buffer[i];
        if (c >= 32 && c <= 126) {
            Serial.print(c);
        } else {
            Serial.print('?');
        }
    }
    Serial.println();
}


static const char* deviceEventLabel(uint32_t which) {
    switch (which) {
        case com_gymjot_cuff_DeviceEvent_status_tag: return "status";
        case com_gymjot_cuff_DeviceEvent_boot_tag: return "boot";
        case com_gymjot_cuff_DeviceEvent_power_event_tag: return "power";
        case com_gymjot_cuff_DeviceEvent_snapshot_tag: return "snapshot";
        case com_gymjot_cuff_DeviceEvent_ota_status_tag: return "ota_status";
        case com_gymjot_cuff_DeviceEvent_tag_tag: return "tag";
        case com_gymjot_cuff_DeviceEvent_exercise_request_tag: return "exercise_request";
        case com_gymjot_cuff_DeviceEvent_exercise_broadcast_tag: return "exercise_broadcast";
        case com_gymjot_cuff_DeviceEvent_exercise_ready_tag: return "exercise_ready";
        case com_gymjot_cuff_DeviceEvent_scan_tag: return "scan";
        case com_gymjot_cuff_DeviceEvent_rep_tag: return "rep";
        case com_gymjot_cuff_DeviceEvent_photo_meta_tag: return "photo_meta";
        case com_gymjot_cuff_DeviceEvent_photo_chunk_tag: return "photo_chunk";
        case com_gymjot_cuff_DeviceEvent_video_frame_tag: return "video_frame";
        case com_gymjot_cuff_DeviceEvent_apriltag_detected_tag: return "apriltag_detected";
        case com_gymjot_cuff_DeviceEvent_motion_detected_tag: return "motion_detected";
        default: return "unknown";
    }
}

static void logEventSummary(const com_gymjot_cuff_DeviceEvent& event) {
    Serial.print("[BLE] notify event=");
    Serial.println(deviceEventLabel(event.which_event));

    switch (event.which_event) {
        case com_gymjot_cuff_DeviceEvent_status_tag: {
            logCStringField("[BLE]   label=", event.event.status.status_label);
            Serial.print("[BLE]   mode=");
            Serial.println(protoModeLabel(event.event.status.mode));
            Serial.print("[BLE]   fps=");
            Serial.println(event.event.status.fps, 2);
            Serial.print("[BLE]   test_mode=");
            Serial.println(boolLabel(event.event.status.test_mode));
            break;
        }
        case com_gymjot_cuff_DeviceEvent_boot_tag: {
            Serial.print("[BLE]   test_mode=");
            Serial.println(boolLabel(event.event.boot.test_mode));
            Serial.print("[BLE]   mode=");
            Serial.println(protoModeLabel(event.event.boot.mode));
            Serial.print("[BLE]   fps=");
            Serial.println(event.event.boot.fps, 2);
            break;
        }
        case com_gymjot_cuff_DeviceEvent_tag_tag: {
            Serial.print("[BLE]   tag_id=");
            Serial.println(event.event.tag.tag_id);
            Serial.print("[BLE]   from_test_mode=");
            Serial.println(boolLabel(event.event.tag.from_test_mode));
            break;
        }
        case com_gymjot_cuff_DeviceEvent_exercise_request_tag: {
            Serial.print("[BLE]   tag_id=");
            Serial.println(event.event.exercise_request.tag_id);
            break;
        }
        case com_gymjot_cuff_DeviceEvent_exercise_broadcast_tag: {
            Serial.print("[BLE]   exercise_id=");
            Serial.println(event.event.exercise_broadcast.exercise_id);
            Serial.print("[BLE]   from_test_mode=");
            Serial.println(boolLabel(event.event.exercise_broadcast.from_test_mode));
            logCStringField("[BLE]   name=", event.event.exercise_broadcast.name);
            break;
        }
        case com_gymjot_cuff_DeviceEvent_exercise_ready_tag: {
            Serial.print("[BLE]   exercise_id=");
            Serial.println(event.event.exercise_ready.exercise_id);
            break;
        }
        case com_gymjot_cuff_DeviceEvent_scan_tag: {
#ifdef ARDUINO
            static uint32_t lastScanLogMs = 0;
            uint32_t nowMs = millis();
            if (nowMs - lastScanLogMs < 500) {
                break;
            }
            lastScanLogMs = nowMs;
#endif
            Serial.print("[BLE]   tag_id=");
            Serial.println(event.event.scan.tag_id);
            Serial.print("[BLE]   distance_cm=");
            Serial.println(event.event.scan.distance_cm, 2);
            Serial.print("[BLE]   fps=");
            Serial.println(event.event.scan.fps, 2);
            Serial.print("[BLE]   mode=");
            Serial.println(protoModeLabel(event.event.scan.mode));
            break;
        }
        case com_gymjot_cuff_DeviceEvent_rep_tag: {
            Serial.print("[BLE]   tag_id=");
            Serial.println(event.event.rep.tag_id);
            Serial.print("[BLE]   rep_count=");
            Serial.println(event.event.rep.rep_count);
            break;
        }
        case com_gymjot_cuff_DeviceEvent_photo_meta_tag: {
            Serial.print("[BLE]   session_id=");
            Serial.println(event.event.photo_meta.session_id);
            Serial.print("[BLE]   total_bytes=");
            Serial.println(event.event.photo_meta.total_bytes);
            Serial.print("[BLE]   dimensions=");
            Serial.print(event.event.photo_meta.width);
            Serial.print("x");
            Serial.println(event.event.photo_meta.height);
            logCStringField("[BLE]   mime=", event.event.photo_meta.mime_type);
            break;
        }
        case com_gymjot_cuff_DeviceEvent_photo_chunk_tag: {
#ifdef ARDUINO
            static uint32_t lastChunkLogSession = 0;
            static uint32_t lastChunkLoggedOffset = 0;
#endif
            uint32_t sessionId = event.event.photo_chunk.session_id;
            uint32_t offset = event.event.photo_chunk.offset;
            uint32_t chunkSize = event.event.photo_chunk.data.size;
            bool finalChunk = event.event.photo_chunk.final_chunk;
#ifdef ARDUINO
            bool shouldLog = (offset == 0) || finalChunk;
            if (!shouldLog) {
                if (sessionId != lastChunkLogSession || offset >= lastChunkLoggedOffset + (kPhotoChunkPayloadBytes * 10)) {
                    shouldLog = true;
                }
            }
            if (shouldLog) {
                lastChunkLogSession = sessionId;
                lastChunkLoggedOffset = offset;
#endif
                Serial.print("[BLE]   session_id=");
                Serial.println(sessionId);
                Serial.print("[BLE]   chunk_offset=");
                Serial.println(offset);
                Serial.print("[BLE]   chunk_size=");
                Serial.println(chunkSize);
                Serial.print("[BLE]   final_chunk=");
                Serial.println(boolLabel(finalChunk));
#ifdef ARDUINO
            }
#endif
            break;
        }
        case com_gymjot_cuff_DeviceEvent_snapshot_tag: {
#ifdef ARDUINO
            static uint32_t lastSnapshotLogMs = 0;
            uint32_t nowMs = millis();
            if (nowMs - lastSnapshotLogMs < 5000) {
                break;
            }
            lastSnapshotLogMs = nowMs;
#endif
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(event.event.snapshot.device_id));
            Serial.print("[BLE]   device_id=");
            Serial.println(buffer);
            Serial.print("[BLE]   camera_ready=");
            Serial.println(boolLabel(event.event.snapshot.camera_ready));
            Serial.print("[BLE]   test_mode=");
            Serial.println(boolLabel(event.event.snapshot.test_mode));
            Serial.print("[BLE]   mode=");
            Serial.println(protoModeLabel(event.event.snapshot.mode));
            Serial.print("[BLE]   target_fps=");
            Serial.println(event.event.snapshot.target_fps, 2);
            Serial.print("[BLE]   active_tag_id=");
            Serial.println(event.event.snapshot.active_tag_id);
            break;
        }
        case com_gymjot_cuff_DeviceEvent_power_event_tag: {
            logCStringField("[BLE]   state=", event.event.power_event.state);
            break;
        }
        case com_gymjot_cuff_DeviceEvent_ota_status_tag: {
            Serial.print("[BLE]   phase=");
            Serial.println(static_cast<int>(event.event.ota_status.phase));
            Serial.print("[BLE]   success=");
            Serial.println(boolLabel(event.event.ota_status.success));
            Serial.print("[BLE]   bytes=");
            Serial.print(event.event.ota_status.bytes_transferred);
            Serial.print('/');
            Serial.println(event.event.ota_status.total_bytes);
            logCStringField("[BLE]   message=", event.event.ota_status.message);
            break;
        }
        default:
            break;
    }
}


static uint32_t nextPhotoSessionId() {
    g_photoSessionCounter++;
    if (g_photoSessionCounter == 0) {
        g_photoSessionCounter = 1;
    }
    return g_photoSessionCounter;
}

static uint32_t nextVideoSessionId() {
    g_videoSessionCounter++;
    if (g_videoSessionCounter == 0) {
        g_videoSessionCounter = 1;
    }
    return g_videoSessionCounter;
}

static bool sendVideoFrameChunk(uint32_t sessionId, uint32_t frameNumber, uint32_t totalBytes,
                                 uint32_t offset, const uint8_t* data, size_t length,
                                 bool finalChunk, uint32_t width, uint32_t height, uint64_t nowMs) {
    com_gymjot_cuff_DeviceEvent evt = com_gymjot_cuff_DeviceEvent_init_default;
    evt.timestamp_ms = nowMs;
    evt.which_event = com_gymjot_cuff_DeviceEvent_video_frame_tag;
    evt.event.video_frame.session_id = sessionId;
    evt.event.video_frame.frame_number = frameNumber;
    evt.event.video_frame.total_bytes = totalBytes;
    evt.event.video_frame.offset = offset;
    evt.event.video_frame.width = width;
    evt.event.video_frame.height = height;
    evt.event.video_frame.final_chunk = finalChunk;

    size_t cappedLength = length;
    if (cappedLength > sizeof(evt.event.video_frame.data.bytes)) {
        cappedLength = sizeof(evt.event.video_frame.data.bytes);
    }
    evt.event.video_frame.data.size = static_cast<pb_size_t>(cappedLength);
    std::memcpy(evt.event.video_frame.data.bytes, data, cappedLength);

    return sendEvent(evt);
}

static bool sendAprilTagDetectedEvent(uint32_t tagId, float distanceCm, float decisionMargin,
                                      const apriltag_detection_t* det, uint64_t nowMs) {
    com_gymjot_cuff_DeviceEvent evt = com_gymjot_cuff_DeviceEvent_init_default;
    evt.timestamp_ms = nowMs;
    evt.which_event = com_gymjot_cuff_DeviceEvent_apriltag_detected_tag;
    evt.event.apriltag_detected.tag_id = tagId;
    evt.event.apriltag_detected.distance_cm = distanceCm;
    evt.event.apriltag_detected.decision_margin = decisionMargin;

    // Normalize corner positions (0-1 range)
    if (det) {
        float imgWidth = 160.0f;  // QQVGA width
        float imgHeight = 120.0f; // QQVGA height
        evt.event.apriltag_detected.corner_x1 = det->p[0][0] / imgWidth;
        evt.event.apriltag_detected.corner_y1 = det->p[0][1] / imgHeight;
        evt.event.apriltag_detected.corner_x2 = det->p[1][0] / imgWidth;
        evt.event.apriltag_detected.corner_y2 = det->p[1][1] / imgHeight;
        evt.event.apriltag_detected.corner_x3 = det->p[2][0] / imgWidth;
        evt.event.apriltag_detected.corner_y3 = det->p[2][1] / imgHeight;
        evt.event.apriltag_detected.corner_x4 = det->p[3][0] / imgWidth;
        evt.event.apriltag_detected.corner_y4 = det->p[3][1] / imgHeight;
    }

    return sendEvent(evt);
}

static bool sendMotionDetectedEvent(float motionScore, uint32_t pixelsChanged, uint32_t totalPixels, uint64_t nowMs) {
    com_gymjot_cuff_DeviceEvent evt = com_gymjot_cuff_DeviceEvent_init_default;
    evt.timestamp_ms = nowMs;
    evt.which_event = com_gymjot_cuff_DeviceEvent_motion_detected_tag;
    evt.event.motion_detected.motion_score = motionScore;
    evt.event.motion_detected.pixels_changed = pixelsChanged;
    evt.event.motion_detected.total_pixels = totalPixels;

    return sendEvent(evt);
}

static void sendStatusLabel(const char* label, uint64_t nowMs) {
    com_gymjot_cuff_DeviceEvent evt = com_gymjot_cuff_DeviceEvent_init_default;
    evt.timestamp_ms = nowMs;
    evt.which_event = com_gymjot_cuff_DeviceEvent_status_tag;
    std::memset(evt.event.status.status_label, 0, sizeof(evt.event.status.status_label));
    std::strncpy(evt.event.status.status_label, label, sizeof(evt.event.status.status_label) - 1);
    evt.event.status.mode = g_controller ? toProtoMode(g_controller->mode()) : com_gymjot_cuff_DeviceMode_DEVICE_MODE_IDLE;
    evt.event.status.fps = g_controller ? g_controller->targetFps() : DEFAULT_FPS;
    evt.event.status.test_mode = g_controller && g_controller->testMode();
    sendEvent(evt);
}

// Deferred version - queues status for sending in main loop (doesn't block GATT callback)
static void queueStatusLabel(const char* label, uint64_t nowMs) {
    std::memset(g_deferredStatus.label, 0, sizeof(g_deferredStatus.label));
    std::strncpy(g_deferredStatus.label, label, sizeof(g_deferredStatus.label) - 1);
    g_deferredStatus.timestamp = nowMs;
    g_deferredStatus.pending = true;
}

static void sendPhotoMetaEvent(uint32_t sessionId, uint32_t totalBytes, uint32_t width, uint32_t height, const char* mimeType, uint64_t nowMs) {
    com_gymjot_cuff_DeviceEvent evt = com_gymjot_cuff_DeviceEvent_init_default;
    evt.timestamp_ms = nowMs;
    evt.which_event = com_gymjot_cuff_DeviceEvent_photo_meta_tag;
    evt.event.photo_meta.session_id = sessionId;
    evt.event.photo_meta.total_bytes = totalBytes;
    evt.event.photo_meta.width = width;
    evt.event.photo_meta.height = height;
    std::snprintf(evt.event.photo_meta.mime_type, sizeof(evt.event.photo_meta.mime_type), "%s", mimeType);
    sendEvent(evt);
}

static bool sendPhotoChunkEvent(uint32_t sessionId, uint32_t offset, const uint8_t* data, size_t length, bool finalChunk, uint64_t nowMs) {
    com_gymjot_cuff_DeviceEvent evt = com_gymjot_cuff_DeviceEvent_init_default;
    evt.timestamp_ms = nowMs;
    evt.which_event = com_gymjot_cuff_DeviceEvent_photo_chunk_tag;
    evt.event.photo_chunk.session_id = sessionId;
    evt.event.photo_chunk.offset = offset;
    size_t cappedLength = length;
    if (cappedLength > sizeof(evt.event.photo_chunk.data.bytes)) {
        cappedLength = sizeof(evt.event.photo_chunk.data.bytes);
    }
    evt.event.photo_chunk.data.size = static_cast<pb_size_t>(cappedLength);
    std::memcpy(evt.event.photo_chunk.data.bytes, data, cappedLength);
    evt.event.photo_chunk.final_chunk = finalChunk;
    return sendEvent(evt);
}

static bool switchToPhotoCamera(framesize_t frameSize, int quality) {
    camera_config_t config = g_photoCameraConfig;
    config.frame_size = frameSize;
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.print("[PHOTO] esp_camera_init(photo) failed: ");
        Serial.println(err);
        return false;
    }
    sensor_t* sensor = esp_camera_sensor_get();
    if (sensor) {
        sensor->set_framesize(sensor, frameSize);
        sensor->set_pixformat(sensor, PIXFORMAT_JPEG);
        sensor->set_quality(sensor, quality);
    }
    return true;
}

static bool restorePrimaryCamera() {
    esp_err_t err = esp_camera_init(&g_grayscaleCameraConfig);
    if (err != ESP_OK) {
        Serial.print("[PHOTO] Failed to restore camera: ");
        Serial.println(err);
        g_cameraReady = false;
        return false;
    }
    sensor_t* sensor = esp_camera_sensor_get();
    if (sensor) {
        sensor->set_framesize(sensor, g_grayscaleCameraConfig.frame_size);
        sensor->set_pixformat(sensor, PIXFORMAT_GRAYSCALE);
    }
    g_cameraReady = true;
    return true;
}

static void detectMotion(const uint8_t* currentFrame, size_t frameSize, uint32_t width, uint32_t height, uint64_t nowMs) {
    if (!g_videoState.motionEnabled) {
        return;
    }

    // Initialize previous frame buffer if needed
    if (g_videoState.previousFrame == nullptr || g_videoState.previousFrameSize != frameSize) {
        if (g_videoState.previousFrame) {
            free(g_videoState.previousFrame);
        }
        g_videoState.previousFrame = (uint8_t*)malloc(frameSize);
        if (!g_videoState.previousFrame) {
            Serial.println("[MOTION] Failed to allocate buffer");
            return;
        }
        g_videoState.previousFrameSize = frameSize;
        g_videoState.frameWidth = width;
        g_videoState.frameHeight = height;
        std::memcpy(g_videoState.previousFrame, currentFrame, frameSize);
        return;
    }

    // Calculate motion (pixel differences)
    uint32_t pixelsChanged = 0;
    const uint32_t threshold = 15;  // Motion threshold (0-255)

    for (size_t i = 0; i < frameSize; ++i) {
        int diff = abs(static_cast<int>(currentFrame[i]) - static_cast<int>(g_videoState.previousFrame[i]));
        if (diff > threshold) {
            pixelsChanged++;
        }
    }

    // Update previous frame
    std::memcpy(g_videoState.previousFrame, currentFrame, frameSize);

    // Calculate motion score (percentage)
    uint32_t totalPixels = width * height;
    float motionScore = (static_cast<float>(pixelsChanged) / static_cast<float>(totalPixels)) * 100.0f;

    // Send motion event if significant motion detected (>5% of pixels changed)
    if (motionScore > 5.0f) {
        Serial.print("[MOTION] Motion detected: ");
        Serial.print(motionScore);
        Serial.print("% (");
        Serial.print(pixelsChanged);
        Serial.print("/");
        Serial.print(totalPixels);
        Serial.println(" pixels)");
        sendMotionDetectedEvent(motionScore, pixelsChanged, totalPixels, nowMs);
    }
}

static bool captureAndStreamVideoFrame(uint64_t nowMs) {
    if (!g_cameraReady || !g_tagDetector) {
        Serial.println("[VIDEO] Camera or detector not ready");
        return false;
    }

    const uint64_t captureStartMs = millis();
    g_heapMonitor.update("apriltag-capture-start", captureStartMs);

    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("[VIDEO] Failed to grab frame");
        return false;
    }

    g_videoState.frameNumber++;

    // AprilTag detection if enabled
    apriltag_detection_t* bestDetection = nullptr;
    float bestDistance = 0.0f;
    double bestMargin = 0.0;

    if (g_videoState.apriltagEnabled) {
        image_u8_t image = {
            static_cast<int32_t>(fb->width),
            static_cast<int32_t>(fb->height),
            static_cast<int32_t>(fb->width),
            fb->buf
        };
        zarray_t* detections = apriltag_detector_detect(g_tagDetector, &image);

        for (int i = 0; i < zarray_size(detections); ++i) {
            apriltag_detection_t* det = nullptr;
            zarray_get(detections, i, &det);
            if (!det || det->family != g_tagFamily) {
                continue;
            }
            if (det->decision_margin >= APRILTAG_MIN_DECISION_MARGIN) {
                if (!bestDetection || det->decision_margin > bestMargin) {
                    bestDetection = det;
                    bestMargin = det->decision_margin;
                    bestDistance = computeDetectionDistance(det);
                }
            }
        }

        if (bestDetection) {
            Serial.print("[VIDEO] AprilTag detected: ID=");
            Serial.print(bestDetection->id);
            Serial.print(", distance=");
            Serial.print(bestDistance);
            Serial.print("cm, margin=");
            Serial.println(bestMargin);

            sendAprilTagDetectedEvent(bestDetection->id, bestDistance, bestMargin, bestDetection, nowMs);
        }

        apriltag_detections_destroy(detections);
    }

    // Motion detection
    if (g_videoState.motionEnabled) {
        detectMotion(fb->buf, fb->len, fb->width, fb->height, nowMs);
    }

    // Stream frame as JPEG chunks
    // For grayscale, we need to convert to JPEG first or send raw with lower quality
    // For now, send the frame buffer directly (assume JPEG format from camera)
    const size_t attPayload = (g_currentMtu > 3) ? static_cast<size_t>(g_currentMtu - 3) : static_cast<size_t>(20);
    constexpr size_t kVideoChunkProtoOverhead = 48;

    if (attPayload <= kVideoChunkProtoOverhead) {
        esp_camera_fb_return(fb);
        return false;
    }

    size_t chunkLimit = 160;  // Max chunk size for video
    size_t mtuLimitedChunk = attPayload - kVideoChunkProtoOverhead;
    if (mtuLimitedChunk < chunkLimit) {
        chunkLimit = mtuLimitedChunk;
    }

    size_t offset = 0;
    while (offset < fb->len) {
        size_t remaining = fb->len - offset;
        size_t chunk = remaining < chunkLimit ? remaining : chunkLimit;

        bool sent = sendVideoFrameChunk(
            g_videoState.sessionId,
            g_videoState.frameNumber,
            fb->len,
            static_cast<uint32_t>(offset),
            fb->buf + offset,
            chunk,
            (offset + chunk) >= fb->len,
            fb->width,
            fb->height,
            nowMs
        );

        if (!sent) {
            Serial.println("[VIDEO] Frame chunk send failed");
            esp_camera_fb_return(fb);
            return false;
        }

        offset += chunk;
        delay(5);  // Small delay between chunks
    }

    esp_camera_fb_return(fb);
    return true;
}

static bool captureAndSendPhoto(uint32_t sessionId, bool highResolution, uint64_t requestTimeMs) {
    if (!g_cameraConfigInitialized) {
        Serial.println("[PHOTO] Camera configuration not initialized");
        g_cameraReady = false;
        return false;
    }

    uint64_t startMs = millis();
    Serial.println("[PHOTO] ========================================");
    Serial.print("[PHOTO] Capturing photo (session=");
    Serial.print(sessionId);
    Serial.print(", high_res=");
    Serial.print(boolLabel(highResolution));
    Serial.println(")");
    Serial.print("[PHOTO] Free heap before: ");
    Serial.print(ESP.getFreeHeap());
    Serial.println(" bytes");
    Serial.print("[PHOTO] Free PSRAM before: ");
    Serial.print(ESP.getFreePsram());
    Serial.println(" bytes");

    g_cameraReady = false;

    // Reset watchdog before potentially long operation
    esp_task_wdt_reset();

    esp_err_t err = esp_camera_deinit();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        Serial.print("[PHOTO] esp_camera_deinit returned ");
        Serial.println(err);
    }

    // Small delay to allow camera hardware to fully release
    delay(50);

    framesize_t frameSize = highResolution ? kPhotoFrameSizeHigh : kPhotoFrameSizeLow;
    int quality = highResolution ? kPhotoQualityHigh : kPhotoQualityLow;

    Serial.print("[PHOTO] Requested size: ");
    Serial.print(frameSize == FRAMESIZE_QVGA ? "QVGA (320x240)" : "QQVGA (160x120)");
    Serial.print(", quality: ");
    Serial.println(quality);

    if (!switchToPhotoCamera(frameSize, quality)) {
        Serial.println("[PHOTO] !!! Failed to switch to photo camera !!!");
        restorePrimaryCamera();
        return false;
    }

    Serial.print("[PHOTO] Free heap after camera init: ");
    Serial.print(ESP.getFreeHeap());
    Serial.println(" bytes");

    uint64_t captureStartMs = millis();
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("[PHOTO] !!! Failed to capture frame buffer !!!");
        Serial.print("[PHOTO] Free heap on failure: ");
        Serial.print(ESP.getFreeHeap());
        Serial.println(" bytes");
        esp_camera_deinit();
        restorePrimaryCamera();
        return false;
    }

    uint64_t captureEndMs = millis();
    Serial.print("[PHOTO] Captured ");
    Serial.print(fb->len);
    Serial.print(" bytes (");
    Serial.print(fb->width);
    Serial.print("x");
    Serial.print(fb->height);
    Serial.print(") in ");
    Serial.print(captureEndMs - captureStartMs);
    Serial.println("ms");
    Serial.print("[PHOTO] Free heap after capture: ");
    Serial.print(ESP.getFreeHeap());
    Serial.println(" bytes");
    Serial.print("[PHOTO] Current BLE MTU: ");
    Serial.print(g_currentMtu);
    Serial.println(" bytes");

    Serial.println("[PHOTO] Sending PhotoMetaEvent...");
    sendPhotoMetaEvent(sessionId, fb->len, fb->width, fb->height, kPhotoMimeType, millis());
    Serial.println("[PHOTO] PhotoMetaEvent sent");

    // Reset watchdog before potentially long transfer
    esp_task_wdt_reset();

    const size_t attPayload = (g_currentMtu > 3) ? static_cast<size_t>(g_currentMtu - 3) : static_cast<size_t>(20);
    constexpr size_t kPhotoChunkProtoOverhead = 32;
    if (attPayload <= kPhotoChunkProtoOverhead) {
        Serial.print("[PHOTO] MTU too small for photo transfer: ");
        Serial.println(g_currentMtu);
        sendStatusLabel("photo-error-mtu", millis());
        esp_camera_fb_return(fb);
        esp_camera_deinit();
        restorePrimaryCamera();
        return false;
    }

    size_t chunkLimit = kPhotoChunkPayloadBytes;
    size_t mtuLimitedChunk = attPayload - kPhotoChunkProtoOverhead;
    if (mtuLimitedChunk < chunkLimit) {
        chunkLimit = mtuLimitedChunk;
    }

    Serial.print("[PHOTO] Using chunk size: ");
    Serial.print(chunkLimit);
    Serial.println(" bytes");

    size_t offset = 0;
    size_t chunkCount = 0;
    uint64_t transferStartMs = millis();
    uint64_t lastProgressMs = transferStartMs;

    while (offset < fb->len) {
        size_t remaining = fb->len - offset;
        size_t chunk = remaining < chunkLimit ? remaining : chunkLimit;

        bool sent = sendPhotoChunkEvent(sessionId, static_cast<uint32_t>(offset), fb->buf + offset, chunk, (offset + chunk) >= fb->len, millis());
        if (!sent && g_clientConnected) {
            Serial.println("[PHOTO] !!! Chunk send failed but client still connected !!!");
        } else if (!sent) {
            Serial.println("[PHOTO] !!! Chunk send failed - client disconnected !!!");
            esp_camera_fb_return(fb);
            esp_camera_deinit();
            restorePrimaryCamera();
            return false;
        }

        offset += chunk;
        chunkCount++;

        if (millis() - lastProgressMs > 1000) {
            Serial.print("[PHOTO] Progress: ");
            Serial.print((offset * 100) / fb->len);
            Serial.print("% (");
            Serial.print(offset);
            Serial.print("/");
            Serial.print(fb->len);
            Serial.println(" bytes)");
            lastProgressMs = millis();
        }

        delay(35);
        yield();

        if (chunkCount % 10 == 0) {
            esp_task_wdt_reset();
        }
    }

    uint64_t transferEndMs = millis();
    Serial.print("[PHOTO] Sent ");
    Serial.print(chunkCount);
    Serial.print(" chunks in ");
    Serial.print(transferEndMs - transferStartMs);
    Serial.print("ms (avg ");
    Serial.print((transferEndMs - transferStartMs) / chunkCount);
    Serial.println("ms/chunk)");

    esp_camera_fb_return(fb);

    Serial.print("[PHOTO] Free heap after fb_return: ");
    Serial.print(ESP.getFreeHeap());
    Serial.println(" bytes");

    err = esp_camera_deinit();
    if (err != ESP_OK) {
        Serial.print("[PHOTO] esp_camera_deinit(photo) returned ");
        Serial.println(err);
    }

    // Small delay before reinitializing primary camera
    delay(50);

    bool restored = restorePrimaryCamera();
    if (!restored) {
        Serial.println("[PHOTO] !!! Failed to restore primary camera !!!");
    }

    uint64_t endMs = millis();
    Serial.print("[PHOTO] Total operation time: ");
    Serial.print(endMs - startMs);
    Serial.println("ms");
    Serial.print("[PHOTO] Free heap after restore: ");
    Serial.print(ESP.getFreeHeap());
    Serial.println(" bytes");
    Serial.print("[PHOTO] Client still connected: ");
    Serial.println(g_clientConnected ? "YES" : "NO");
    Serial.println("[PHOTO] ========================================");

    // Reset watchdog after completing photo operation
    esp_task_wdt_reset();

    return restored;
}

static void handlePendingPhotoRequest(uint64_t nowMs) {
    if (!g_pendingPhotoRequest.pending || g_photoCaptureInProgress) {
        return;
    }

    if (!g_cameraReady) {
        Serial.println("[PHOTO] Camera not ready for capture");
        sendStatusLabel("photo-error-camera", nowMs);
        g_pendingPhotoRequest.pending = false;
        return;
    }

    if (g_otaInProgress) {
        Serial.println("[PHOTO] Photo capture blocked during OTA");
        sendStatusLabel("photo-error-ota", nowMs);
        g_pendingPhotoRequest.pending = false;
        return;
    }

    const size_t attPayload = (g_currentMtu > 3) ? static_cast<size_t>(g_currentMtu - 3) : static_cast<size_t>(20);
    if (attPayload <= 32) {
        Serial.print("[PHOTO] MTU too small (");
        Serial.print(g_currentMtu);
        Serial.println(" bytes)");
        sendStatusLabel("photo-error-mtu", nowMs);
        g_pendingPhotoRequest.pending = false;
        return;
    }

    Serial.println("[PHOTO] === STARTING PHOTO CAPTURE ===");
    Serial.println("[PHOTO] AprilTag detection PAUSED during photo capture");
    Serial.print("[PHOTO] Session ID: ");
    Serial.println(g_pendingPhotoRequest.session_id);
    Serial.print("[PHOTO] High resolution: ");
    Serial.println(g_pendingPhotoRequest.high_resolution ? "true" : "false");

    g_photoCaptureInProgress = true;
    bool highRes = g_pendingPhotoRequest.high_resolution;
    uint32_t sessionId = g_pendingPhotoRequest.session_id;
    g_pendingPhotoRequest.pending = false;

    Serial.println("[PHOTO] Sending photo-start status...");
    sendStatusLabel("photo-start", nowMs);
    Serial.println("[PHOTO] Calling captureAndSendPhoto...");
    bool success = captureAndSendPhoto(sessionId, highRes, nowMs);
    uint64_t finishMs = millis();

    Serial.println("[PHOTO] === PHOTO CAPTURE COMPLETE ===");
    Serial.print("[PHOTO] Success: ");
    Serial.println(success ? "true" : "false");
    Serial.println("[PHOTO] AprilTag detection RESUMED");

    Serial.print("[PHOTO] Sending ");
    Serial.print(success ? "photo-complete" : "photo-error");
    Serial.println(" status...");
    sendStatusLabel(success ? "photo-complete" : "photo-error", finishMs);
    g_photoCaptureInProgress = false;
}

class RxCallback : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo) override {
        // Track first ATT request
        if (g_firstAttRequestTimestamp == 0) {
            g_firstAttRequestTimestamp = millis();
            Serial.print("[INSTR] First ATT request at +");
            Serial.print(g_firstAttRequestTimestamp - g_connectTimestamp);
            Serial.println("ms");
        }

        // Check if connection is encrypted - security request flow
        if (!connInfo.isEncrypted()) {
            Serial.println("!!! WRITE ATTEMPTED ON UNENCRYPTED CONNECTION !!!");
            Serial.println("Requesting encryption/pairing...");

            // Request security upgrade (trigger pairing)
            NimBLEDevice::startSecurity(connInfo.getConnHandle());

            Serial.println("Please complete pairing and retry the command");
            return;
        }

        std::string val = characteristic->getValue();
        if (val.size() < kLengthPrefixBytes) {
            Serial.println("<- command too short");
            return;
        }

        const auto* data = reinterpret_cast<const uint8_t*>(val.data());
        const uint16_t expected = static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
        const size_t available = val.size() - kLengthPrefixBytes;
        if (expected != available) {
            Serial.println("<- length mismatch");
            return;
        }

        processCommand(data + kLengthPrefixBytes, available);
    }
};

class SnapshotCallback : public NimBLECharacteristicCallbacks {
    void onRead(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
        updateSnapshotCharacteristic(millis());
        if (g_snapshotChar && g_snapshotChar != characteristic) {
            characteristic->setValue(g_snapshotChar->getValue());
        }
    }
};

class InfoCallback : public NimBLECharacteristicCallbacks {
    void onRead(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
        if (!g_identity) {
            g_identity = &gymjot::deviceIdentity();
        }
        std::string info = buildInfoString();
        characteristic->setValue(info);
    }
};

class ServerCallback : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
        g_clientConnected = true;
        g_connectTimestamp = millis();
        g_lastConnectionTime = g_connectTimestamp;

        Serial.println("=== BLE CLIENT CONNECTED ===");
        Serial.print("[INSTR] Connect timestamp: ");
        Serial.println(g_connectTimestamp);
        Serial.print("Client address: ");
        Serial.println(connInfo.getAddress().toString().c_str());
        Serial.print("Connection ID: ");
        Serial.println(connInfo.getConnHandle());

        // Get initial connection parameters
        g_currentConnInterval = connInfo.getConnInterval();
        g_currentConnLatency = connInfo.getConnLatency();
        g_currentSupervisionTimeout = connInfo.getConnTimeout();

        Serial.print("[INSTR] Initial conn params: interval=");
        Serial.print(g_currentConnInterval * 1.25f);
        Serial.print("ms, latency=");
        Serial.print(g_currentConnLatency);
        Serial.print(", timeout=");
        Serial.print(g_currentSupervisionTimeout * 10);
        Serial.println("ms");

        // Reset telemetry timestamps
        g_mtuNegotiatedTimestamp = 0;
        g_connParamsUpdatedTimestamp = 0;
        g_firstAttRequestTimestamp = 0;

        // DON'T update connection params, MTU, or PHY yet
        // Wait for GATT discovery to complete first
        Serial.println("Waiting for GATT discovery, MTU negotiation, and pairing...");
    }

    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
        g_clientConnected = false;
        g_lastDisconnectReason = reason;
        uint64_t disconnectTime = millis();

        Serial.println("=== BLE CLIENT DISCONNECTED ===");
        Serial.print("[INSTR] Disconnect timestamp: ");
        Serial.println(disconnectTime);
        Serial.print("Connection duration: ");
        Serial.print((disconnectTime - g_lastConnectionTime) / 1000);
        Serial.println(" seconds");
        Serial.print("Reason code: 0x");
        Serial.print(reason, HEX);
        Serial.print(" - ");

        // Check for authentication/pairing failures - auto-clear bonds
        bool pairingFailed = false;

        // Decode disconnect reason
        switch(reason) {
            case 0x05:  // Authentication failure
                Serial.println("AUTHENTICATION FAILURE");
                pairingFailed = true;
                break;
            case 0x06:  // PIN or key missing
                Serial.println("PIN/KEY MISSING");
                pairingFailed = true;
                break;
            case 0x08:
                Serial.println("Connection timeout");
                break;
            case 0x13:
                Serial.println("Remote user terminated");
                break;
            case 0x16:
                Serial.println("Connection terminated by local host");
                break;
            case 0x3D:  // Connection failed to establish (often pairing timeout)
                Serial.println("Connection failed to establish (possibly pairing timeout)");
                pairingFailed = true;
                break;
            case 0x3E:
                Serial.println("LMP response timeout");
                break;
            case 0x22:
                Serial.println("LMP error / Connection terminated");
                break;
            default:
                Serial.println("Other/Unknown");
                break;
        }

        // Auto-recovery: Clear bonds on authentication failure
        // Don't tighten security on CONN_TIMEOUT or AUTH_FAILURE - allow fresh pairing
        if (pairingFailed) {
            Serial.println("=== AUTO-RECOVERY: Clearing bonds ===");
            NimBLEDevice::deleteAllBonds();
            Serial.println("All bonds cleared - ready for fresh pairing");
        }

        // Restart advertising IMMEDIATELY (within <=500ms)
        // NO delays, NO heavy work (event sending moved to loop)
        Serial.print("[INSTR] Restarting advertising at +");
        Serial.print(millis() - disconnectTime);
        Serial.println("ms...");

        if (NimBLEDevice::startAdvertising()) {
            Serial.println("OK - Device discoverable");
        } else {
            Serial.println("FAILED - retrying");
            NimBLEDevice::startAdvertising();
        }
    }

    void onMTUChange(uint16_t MTU, NimBLEConnInfo& connInfo) override {
        g_currentMtu = MTU;
        g_mtuNegotiatedTimestamp = millis();

        Serial.print("[INSTR] MTU negotiated: ");
        Serial.print(MTU);
        Serial.print(" bytes at +");
        Serial.print(g_mtuNegotiatedTimestamp - g_connectTimestamp);
        Serial.println("ms");
    }

    void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
        Serial.print("[INSTR] Authentication complete at +");
        Serial.print(millis() - g_connectTimestamp);
        Serial.print("ms, encrypted=");
        Serial.print(connInfo.isEncrypted());
        Serial.print(", authenticated=");
        Serial.println(connInfo.isAuthenticated());
    }

    void onConnParamsUpdate(NimBLEConnInfo& connInfo) override {
        g_currentConnInterval = connInfo.getConnInterval();
        g_currentConnLatency = connInfo.getConnLatency();
        g_currentSupervisionTimeout = connInfo.getConnTimeout();
        g_connParamsUpdatedTimestamp = millis();

        Serial.print("[INSTR] Conn params updated at +");
        Serial.print(g_connParamsUpdatedTimestamp - g_connectTimestamp);
        Serial.print("ms: interval=");
        Serial.print(g_currentConnInterval * 1.25f);
        Serial.print("ms, latency=");
        Serial.print(g_currentConnLatency);
        Serial.print(", timeout=");
        Serial.print(g_currentSupervisionTimeout * 10);
        Serial.println("ms");

        // Verify supervision timeout rule: timeout >= 2 * interval * (1 + latency) * 3
        float minTimeoutMs = 2.0f * (g_currentConnInterval * 1.25f) * (1 + g_currentConnLatency) * 3.0f;
        float actualTimeoutMs = g_currentSupervisionTimeout * 10.0f;

        if (actualTimeoutMs < minTimeoutMs) {
            Serial.print("[WARNING] Supervision timeout too low! Recommended: >=");
            Serial.print(minTimeoutMs);
            Serial.print("ms, actual: ");
            Serial.print(actualTimeoutMs);
            Serial.println("ms");
        }
    }
};


static void fillSnapshot(com_gymjot_cuff_SnapshotEvent& snapshot) {
    if (!g_identity) {
        g_identity = &gymjot::deviceIdentity();
    }
    snapshot.device_id = g_identity->deviceId;
    std::memset(snapshot.name, 0, sizeof(snapshot.name));
    std::strncpy(snapshot.name, g_identity->name.c_str(), sizeof(snapshot.name) - 1);
    snapshot.camera_ready = g_cameraReady;
    snapshot.ota_in_progress = g_otaInProgress;

    if (g_controller) {
        snapshot.mode = toProtoMode(g_controller->mode());
        snapshot.test_mode = g_controller->testMode();
        snapshot.target_fps = g_controller->targetFps();
        snapshot.loiter_fps = g_controller->loiterFps();
        snapshot.min_travel_cm = g_controller->minTravelCm();
        snapshot.max_rep_idle_ms = g_controller->maxRepIdleMs();
        snapshot.active_tag_id = g_controller->session().tagId;
    } else {
        snapshot.mode = com_gymjot_cuff_DeviceMode_DEVICE_MODE_IDLE;
        snapshot.test_mode = false;
        snapshot.target_fps = DEFAULT_FPS;
        snapshot.loiter_fps = LOITER_FPS;
        snapshot.min_travel_cm = DEFAULT_MIN_REP_TRAVEL_CM;
        snapshot.max_rep_idle_ms = DEFAULT_MAX_REP_IDLE_MS;
        snapshot.active_tag_id = 0;
    }

    // Connection telemetry (uncomment after regenerating protobuf)
    // snapshot.ble_connected = g_clientConnected;
    // snapshot.ble_mtu = g_currentMtu;
    // snapshot.conn_interval_ms = static_cast<uint32_t>(g_currentConnInterval * 1.25f * 100);  // x100 for precision
    // snapshot.conn_latency = g_currentConnLatency;
    // snapshot.supervision_timeout_ms = g_currentSupervisionTimeout * 10;
    // snapshot.last_disconnect_reason = g_lastDisconnectReason;
    // snapshot.bonded_count = NimBLEDevice::getNumBonds();
}

static void updateSnapshotCharacteristic(uint64_t nowMs) {
    (void)nowMs;
    if (!g_snapshotChar) {
        return;
    }
    com_gymjot_cuff_SnapshotEvent snapshot = com_gymjot_cuff_SnapshotEvent_init_default;
    fillSnapshot(snapshot);
    std::array<uint8_t, kLengthPrefixBytes + kProtoBufferSize> buffer{};
    size_t totalLen = 0;
    if (!encodeWithLength(com_gymjot_cuff_SnapshotEvent_fields, &snapshot, buffer, totalLen)) {
        return;
    }
    g_snapshotChar->setValue(buffer.data(), totalLen);
}

static void sendSnapshotEvent(uint64_t nowMs) {
    com_gymjot_cuff_DeviceEvent evt = com_gymjot_cuff_DeviceEvent_init_default;
    evt.timestamp_ms = nowMs;
    evt.which_event = com_gymjot_cuff_DeviceEvent_snapshot_tag;
    fillSnapshot(evt.event.snapshot);
    sendEvent(evt);
}

static void sendOtaStatus(com_gymjot_cuff_OtaPhase phase, const char* message, bool success, uint32_t transferred, uint32_t total) {
    com_gymjot_cuff_DeviceEvent evt = com_gymjot_cuff_DeviceEvent_init_default;
    evt.timestamp_ms = millis();
    evt.which_event = com_gymjot_cuff_DeviceEvent_ota_status_tag;
    evt.event.ota_status.phase = phase;
    evt.event.ota_status.success = success;
    evt.event.ota_status.bytes_transferred = transferred;
    evt.event.ota_status.total_bytes = total;
    if (message) {
        std::memset(evt.event.ota_status.message, 0, sizeof(evt.event.ota_status.message));
        std::strncpy(evt.event.ota_status.message, message, sizeof(evt.event.ota_status.message) - 1);
    }
    sendEvent(evt);
}

static void sendPowerEvent(const char* state, uint64_t nowMs) {
    com_gymjot_cuff_DeviceEvent evt = com_gymjot_cuff_DeviceEvent_init_default;
    evt.timestamp_ms = nowMs;
    evt.which_event = com_gymjot_cuff_DeviceEvent_power_event_tag;
    if (state) {
        std::memset(evt.event.power_event.state, 0, sizeof(evt.event.power_event.state));
        std::strncpy(evt.event.power_event.state, state, sizeof(evt.event.power_event.state) - 1);
    }
    sendEvent(evt);
}

class OtaCallback : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
        const std::string& value = characteristic->getValue();
        g_otaInProgress = true;
        g_otaReceivedBytes += static_cast<uint32_t>(value.size());
        updateSnapshotCharacteristic(millis());
        sendOtaStatus(com_gymjot_cuff_OtaPhase_OTA_PHASE_ERROR, "Use DeviceCommand OTA interface", false, g_otaReceivedBytes, g_otaTotalBytes);
        g_otaInProgress = false;
        updateSnapshotCharacteristic(millis());
    }
};
static bool sendEvent(const com_gymjot_cuff_DeviceEvent& event) {
    if (!g_tx) {
        static bool warnedMissingTx = false;
        if (!warnedMissingTx) {
            Serial.println("[BLE] TX characteristic not ready");
            warnedMissingTx = true;
        }
        return false;
    }

    // Don't try to send if no client is connected
    if (!g_clientConnected) {
    #ifdef ARDUINO
        static uint32_t lastSkipLogMs = 0;
        uint32_t nowMs = millis();
        if (nowMs - lastSkipLogMs > 1000) {
            Serial.println("[BLE] skip notify (no client connected)");
            lastSkipLogMs = nowMs;
        }
    #else
        Serial.println("[BLE] skip notify (no client connected)");
    #endif
        return false;
    }

    std::array<uint8_t, kLengthPrefixBytes + kProtoBufferSize> buffer{};
    size_t totalLen = 0;
    if (!encodeWithLength(com_gymjot_cuff_DeviceEvent_fields, &event, buffer, totalLen)) {
        return false;
    }

    g_tx->setValue(buffer.data(), totalLen);
    if (!g_tx->notify()) {
        Serial.println("notify failed (client may have disconnected)");
        return false;
    }

    logPacket(totalLen);
    logEventSummary(event);
    return true;
}

static void setupController() {
    ControllerConfig cfg;
    cfg.defaultTestMode = TEST_MODE_DEFAULT;
    cfg.defaultFps = DEFAULT_FPS;
    cfg.loiterFps = LOITER_FPS;
    cfg.tagLostMs = APRILTAG_LOST_MS;
    cfg.defaultMinTravelCm = DEFAULT_MIN_REP_TRAVEL_CM;
    cfg.maxRepIdleMs = DEFAULT_MAX_REP_IDLE_MS;
    cfg.testExerciseId = TEST_EXERCISE_ID;
    cfg.testExerciseName = TEST_EXERCISE_NAME;
    cfg.testExerciseMetadata = defaultTestExerciseMetadata();

    g_controller = std::make_unique<CuffController>(cfg, sendEvent);
}

static bool setupCamera() {
    // Ensure the camera hardware starts from a clean state
    esp_camera_deinit();
    delay(50);

    camera_config_t config = {};
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 10000000;
    config.pixel_format = PIXFORMAT_GRAYSCALE;
    config.frame_size = FRAMESIZE_QQVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

    g_grayscaleCameraConfig = config;
    g_photoCameraConfig = config;
    g_photoCameraConfig.pixel_format = PIXFORMAT_JPEG;
    g_photoCameraConfig.frame_size = kPhotoFrameSizeHigh;
    g_photoCameraConfig.fb_count = 1;
    g_photoCameraConfig.jpeg_quality = kPhotoQualityHigh;

    if (esp_camera_init(&g_grayscaleCameraConfig) != ESP_OK) {
        Serial.println("Camera init failed");
        g_cameraConfigInitialized = false;
        return false;
    }

    sensor_t* sensor = esp_camera_sensor_get();
    if (sensor) {
        sensor->set_framesize(sensor, g_grayscaleCameraConfig.frame_size);
        sensor->set_pixformat(sensor, PIXFORMAT_GRAYSCALE);
    }

    g_cameraConfigInitialized = true;
    Serial.println("Camera ready");
    return true;
}

static bool setupAprilTagDetector() {
    g_tagFamily = tagCircle49h12_create();
    if (!g_tagFamily) {
        Serial.println("Failed to create tag family");
        return false;
    }

    g_tagDetector = apriltag_detector_create();
    if (!g_tagDetector) {
        Serial.println("Failed to create AprilTag detector");
        return false;
    }

    g_tagDetector->nthreads = 1;
    g_tagDetector->quad_decimate = APRILTAG_QUAD_DECIMATE;
    g_tagDetector->quad_sigma = APRILTAG_QUAD_SIGMA;
    g_tagDetector->refine_edges = APRILTAG_REFINE_EDGES;
    g_tagDetector->decode_sharpening = 0.25f;
    apriltag_detector_add_family(g_tagDetector, g_tagFamily);
    return true;
}

static void setupBLE() {
    if (!g_identity) {
        g_identity = &gymjot::deviceIdentity();
    }

    Serial.println("=== BLE INITIALIZATION ===");
    Serial.print("Device name: ");
    Serial.println(g_identity->name.c_str());
    Serial.print("Device ID: 0x");
    Serial.println((unsigned long)g_identity->deviceId, HEX);
    Serial.print("Passkey: ");
    Serial.println(g_identity->passkey);
    Serial.println();

    NimBLEDevice::init(g_identity->name.c_str());
    NimBLEDevice::setDeviceName(g_identity->name.c_str());
    NimBLEDevice::setPower(ESP_PWR_LVL_N12);
    NimBLEDevice::setMTU(247);

    // Security: require bonding, MITM protection, and secure connections
    NimBLEDevice::setSecurityAuth(true, true, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);  // Device displays passkey
    NimBLEDevice::setSecurityPasskey(g_identity->passkey);

    Serial.println("Passkey configured for pairing");

    Serial.println("Security settings:");
    Serial.println("  - Authentication: REQUIRED");
    Serial.println("  - Bonding: REQUIRED");
    Serial.println("  - Encryption: REQUIRED");
    Serial.println("  - IO Capability: NoInputNoOutput");
    Serial.println("  - MTU: 247 bytes");
    Serial.println("  - TX Power: -12 dBm");

    g_server = NimBLEDevice::createServer();
    g_server->setCallbacks(new ServerCallback());
    NimBLEService* service = g_server->createService(SERVICE_UUID);

    // Command RX: Require encryption for writes (security), but allow discovery
    g_commandChar = service->createCharacteristic(CHAR_RX_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC);
    g_commandChar->setCallbacks(new RxCallback());

    // Event TX: Allow unencrypted reads for discovery, notifications don't require encryption
    g_tx = service->createCharacteristic(CHAR_TX_UUID,
        NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ);

    // Snapshot: Allow unencrypted reads for discovery
    g_snapshotChar = service->createCharacteristic(CHAR_SNAPSHOT_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    g_snapshotChar->setCallbacks(new SnapshotCallback());

    // Info: Allow unencrypted reads (public device information)
    g_infoChar = service->createCharacteristic(CHAR_INFO_UUID,
        NIMBLE_PROPERTY::READ);
    g_infoChar->setCallbacks(new InfoCallback());

    // OTA: Require encryption for writes (security)
    g_otaChar = service->createCharacteristic(CHAR_OTA_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC);
    g_otaChar->setCallbacks(new OtaCallback());

    service->start();

    Serial.println("BLE Service created:");
    Serial.print("  - Service UUID: ");
    Serial.println(SERVICE_UUID);
    Serial.println("  - Characteristics:");
    Serial.print("    * Command RX: ");
    Serial.println(CHAR_RX_UUID);
    Serial.print("    * Event TX: ");
    Serial.println(CHAR_TX_UUID);
    Serial.print("    * Info: ");
    Serial.println(CHAR_INFO_UUID);
    Serial.print("    * Snapshot: ");
    Serial.println(CHAR_SNAPSHOT_UUID);
    Serial.print("    * OTA: ");
    Serial.println(CHAR_OTA_UUID);

    updateSnapshotCharacteristic(millis());
    if (g_infoChar) {
        g_infoChar->setValue(buildInfoString());
    }

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    NimBLEAdvertisementData advData;
    advData.setName(g_identity->name);
    advData.addServiceUUID(SERVICE_UUID);
    std::string mfg;
    mfg.push_back(static_cast<char>(MANUFACTURER_ID & 0xFF));
    mfg.push_back(static_cast<char>((MANUFACTURER_ID >> 8) & 0xFF));
    uint64_t deviceId = g_identity->deviceId;
    for (int i = 0; i < 8; ++i) {
        mfg.push_back(static_cast<char>((deviceId >> (8 * i)) & 0xFF));
    }
    advData.setManufacturerData(mfg);
    adv->setAdvertisementData(advData);

    NimBLEAdvertisementData scanData;
    scanData.setName(g_identity->name);
    adv->setScanResponseData(scanData);
    adv->setMinInterval(160);
    adv->setMaxInterval(320);

    Serial.println("Advertising configuration:");
    Serial.print("  - Manufacturer ID: 0x");
    Serial.println(MANUFACTURER_ID, HEX);
    Serial.print("  - Device ID in adv data: 0x");
    Serial.println((unsigned long)g_identity->deviceId, HEX);
    Serial.println("  - Min interval: 100ms (160 * 0.625ms)");
    Serial.println("  - Max interval: 200ms (320 * 0.625ms)");
    Serial.println("  - Service UUID included: YES");

    if (!NimBLEDevice::startAdvertising()) {
        Serial.println("ERROR: Failed to start advertising!");
        Serial.println("TROUBLESHOOTING:");
        Serial.println("  1. Check if BLE is already initialized");
        Serial.println("  2. Verify sufficient memory");
        Serial.println("  3. Check for BLE stack errors");
        return;
    }

    Serial.println("=== BLE ADVERTISING STARTED ===");
    Serial.println("Device is now discoverable and ready to pair!");
    Serial.println();
    Serial.println("PAIRING INSTRUCTIONS:");
    Serial.println("1. Scan for BLE devices on your mobile app");
    Serial.print("2. Look for device: ");
    Serial.println(g_identity->name.c_str());
    Serial.print("3. When prompted, enter passkey: ");
    Serial.println(g_identity->passkey);
    Serial.println("4. Watch this serial output for connection status");
    Serial.println("===============================");
}

static float computeDetectionDistance(const apriltag_detection_t* det) {
    apriltag_detection_info_t info;
    info.det = const_cast<apriltag_detection_t*>(det);
    info.tagsize = APRILTAG_TAG_SIZE_M;
    info.fx = APRILTAG_FX;
    info.fy = APRILTAG_FY;
    info.cx = APRILTAG_CX;
    info.cy = APRILTAG_CY;

    apriltag_pose_t pose;
    double err = estimate_tag_pose(&info, &pose);
    (void)err;

    float distanceCm = 0.0f;
    if (pose.t) {
        double tx = matd_get(pose.t, 0, 0);
        double ty = matd_get(pose.t, 1, 0);
        double tz = matd_get(pose.t, 2, 0);
        double norm = std::sqrt(tx * tx + ty * ty + tz * tz);
        distanceCm = static_cast<float>(norm * 100.0);
    }

    if (pose.R) {
        matd_destroy(pose.R);
    }
    if (pose.t) {
        matd_destroy(pose.t);
    }
    return distanceCm;
}


static bool captureAprilTag(AprilTagDetection& detection) {
    if (!g_cameraReady || !g_tagDetector) {
        static uint64_t lastWarn = 0;
        uint64_t now = millis();
        if (now - lastWarn > 5000) {
            Serial.println("[APRILTAG] Detector not ready");
            Serial.print("[APRILTAG] camera_ready=");
            Serial.println(g_cameraReady ? "true" : "false");
            Serial.print("[APRILTAG] detector_ready=");
            Serial.println(g_tagDetector != nullptr ? "true" : "false");
            lastWarn = now;
        }
        return false;
    }

    const uint64_t captureStartMs = millis();
    g_heapMonitor.update("apriltag-capture-start", captureStartMs);

    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
        static uint64_t lastFbError = 0;
        uint64_t now = captureStartMs;
        if (now - lastFbError > 5000) {
            Serial.println("[APRILTAG] Failed to grab camera frame");
            lastFbError = now;
        }
        g_heapMonitor.update("apriltag-capture-no-fb", now);
        return false;
    }

    image_u8_t image = { static_cast<int32_t>(fb->width), static_cast<int32_t>(fb->height), static_cast<int32_t>(fb->width), fb->buf };
    esp_task_wdt_reset();
    zarray_t* detections = apriltag_detector_detect(g_tagDetector, &image);

    apriltag_detection_t* best = nullptr;
    double bestMargin = 0.0;
    const char* wrongFamilyName = nullptr;
    int wrongFamilyCount = 0;
    int totalDetections = zarray_size(detections);

    for (int i = 0; i < totalDetections; ++i) {
        apriltag_detection_t* det = nullptr;
        zarray_get(detections, i, &det);
        if (!det) {
            continue;
        }
        if (det->family != g_tagFamily) {
            wrongFamilyName = det->family ? det->family->name : nullptr;
            ++wrongFamilyCount;
            continue;
        }
        if (!best || det->decision_margin > bestMargin) {
            best = det;
            bestMargin = det->decision_margin;
        }
    }

    bool found = false;
    uint64_t nowMs = millis();
    static uint64_t lastDetectionLogMs = 0;
    static uint32_t lastLoggedTag = 0;
    static uint64_t lastNoDetectionLogMs = 0;
    static uint64_t lastWrongFamilyLogMs = 0;

    // Detection stability tracking
    static uint32_t stableTagId = 0;
    static int stableFrameCount = 0;
    static uint64_t lastLowMarginLogMs = 0;

    if (best && bestMargin >= APRILTAG_MIN_DECISION_MARGIN) {
        uint32_t detectedId = static_cast<uint32_t>(best->id);

        // Check if this is the same tag as previous frames
        if (detectedId == stableTagId) {
            stableFrameCount++;
        } else {
            // New tag detected, reset stability counter
            stableTagId = detectedId;
            stableFrameCount = 1;
        }

        // Only accept detection if it's been stable for required frames
        if (stableFrameCount >= APRILTAG_STABILITY_FRAMES) {
            detection.tagId = detectedId;
            detection.distanceCm = computeDetectionDistance(best);
            found = true;

            g_lastAprilTagDetectionMs = nowMs;
            g_lastAprilTagId = detection.tagId;
            g_lastAprilTagDistanceCm = detection.distanceCm;
            g_lastAprilTagMargin = bestMargin;

            if (detection.tagId != lastLoggedTag || nowMs - lastDetectionLogMs > 2000) {
                Serial.println("[APRILTAG] Detection");
                Serial.print("[APRILTAG] tag_id=");
                Serial.println(detection.tagId);
                Serial.print("[APRILTAG] distance_cm=");
                Serial.println(detection.distanceCm);
                Serial.print("[APRILTAG] decision_margin=");
                Serial.println(bestMargin);
                lastDetectionLogMs = nowMs;
                lastLoggedTag = detection.tagId;
            }
        }
    } else {
        // Reset stability tracking if no valid detection
        stableTagId = 0;
        stableFrameCount = 0;

        // Log low margin detections occasionally for debugging
        if (best && bestMargin < APRILTAG_MIN_DECISION_MARGIN && nowMs - lastLowMarginLogMs > 5000) {
            Serial.print("[APRILTAG] Rejected low-margin detection: id=");
            Serial.print(best->id);
            Serial.print(", margin=");
            Serial.println(bestMargin);
            lastLowMarginLogMs = nowMs;
        } else if (wrongFamilyCount > 0) {
            if (nowMs - lastWrongFamilyLogMs >= kWrongFamilyLogIntervalMs) {
                Serial.print("[APRILTAG] Detected ");
                Serial.print(wrongFamilyCount);
                Serial.println(" tag(s) from a different family");
                if (wrongFamilyName) {
                    Serial.print("[APRILTAG] last_family=");
                    Serial.println(wrongFamilyName);
                }
                lastWrongFamilyLogMs = nowMs;
            }
        } else {
            if (nowMs - lastNoDetectionLogMs >= kNoDetectionLogIntervalMs) {
                Serial.println("[APRILTAG] No tags detected");
                lastNoDetectionLogMs = nowMs;
            }
        }
    }

    apriltag_detections_destroy(detections);
    esp_camera_fb_return(fb);
    g_heapMonitor.update("apriltag-capture-end", millis());
    return found;
}


static void sendBootStatus(uint64_t nowMs) {
    com_gymjot_cuff_DeviceEvent evt = com_gymjot_cuff_DeviceEvent_init_default;
    evt.timestamp_ms = nowMs;
    evt.which_event = com_gymjot_cuff_DeviceEvent_boot_tag;

    auto& boot = evt.event.boot;
    boot.test_mode = g_controller && g_controller->testMode();
    boot.mode = g_controller ? toProtoMode(g_controller->mode()) : com_gymjot_cuff_DeviceMode_DEVICE_MODE_IDLE;
    boot.fps = g_controller ? g_controller->targetFps() : DEFAULT_FPS;

    sendEvent(evt);
}

void processCommand(const uint8_t* data, size_t len) {
    if (!g_controller) {
        return;
    }

    com_gymjot_cuff_DeviceCommand cmd = com_gymjot_cuff_DeviceCommand_init_default;
    pb_istream_t stream = pb_istream_from_buffer(data, len);
    if (!pb_decode(&stream, com_gymjot_cuff_DeviceCommand_fields, &cmd)) {
        Serial.print("decode error: ");
        Serial.println(PB_GET_ERROR(&stream));
        return;
    }

    uint64_t now = millis();
    switch (cmd.which_command) {
        case com_gymjot_cuff_DeviceCommand_set_test_mode_tag:
            g_controller->setTestMode(cmd.command.set_test_mode.enabled, now);
            break;
        case com_gymjot_cuff_DeviceCommand_set_target_fps_tag:
            g_controller->setTargetFps(cmd.command.set_target_fps.fps, now);
            updateSnapshotCharacteristic(now);
            break;
        case com_gymjot_cuff_DeviceCommand_exercise_update_tag: {
            const auto& update = cmd.command.exercise_update;
            ExercisePayload payload;
            payload.id = update.exercise_id;
            payload.name = update.name;
            if (update.set_min_travel_cm) {
                payload.minTravelCm = update.min_travel_cm;
            }
            if (update.set_fps) {
                payload.fps = update.fps;
            }
            if (update.has_metadata) {
                payload.metadata = metadataFromProto(update.metadata);
            }
            g_controller->handleExercisePayload(payload, now);
            updateSnapshotCharacteristic(now);
            break;
        }
        case com_gymjot_cuff_DeviceCommand_reset_reps_tag:
            g_controller->resetReps(now);
            break;
        case com_gymjot_cuff_DeviceCommand_power_tag:
            if (cmd.command.power.shutdown) {
                sendPowerEvent("shutdown", now);
                delay(250);
                NimBLEDevice::stopAdvertising();
                esp_deep_sleep_start();
                return;
            } else {
                sendPowerEvent("power-ignore", now);
            }
            break;
        case com_gymjot_cuff_DeviceCommand_factory_reset_tag:
            if (cmd.command.factory_reset.confirm) {
                sendPowerEvent("factory-reset", now);
                NimBLEDevice::deleteAllBonds();
                gymjot::clearPersistentSettings();
                gymjot::clearDeviceIdentity();
                delay(250);
                ESP.restart();
                return;
            } else {
                sendPowerEvent("factory-reset-cancel", now);
            }
            break;
        case com_gymjot_cuff_DeviceCommand_take_photo_tag: {
            // IMPORTANT: Don't block GATT callback - just queue the request
            // All notifications will be sent from main loop
            if (g_photoCaptureInProgress || g_pendingPhotoRequest.pending) {
                Serial.println("[PHOTO] Capture already in progress");
                queueStatusLabel("photo-busy", now);
                break;
            }
            if (!g_cameraReady) {
                Serial.println("[PHOTO] Capture requested but camera not ready");
                queueStatusLabel("photo-error-camera", now);
                break;
            }
            if (g_otaInProgress) {
                Serial.println("[PHOTO] Capture blocked during OTA");
                queueStatusLabel("photo-error-ota", now);
                break;
            }
            Serial.println("[CMD] Photo request queued - returning immediately from GATT callback");
            g_pendingPhotoRequest.pending = true;
            g_pendingPhotoRequest.high_resolution = cmd.command.take_photo.high_resolution;
            g_pendingPhotoRequest.session_id = nextPhotoSessionId();
            queueStatusLabel("photo-queued", now);
            break;
        }
        case com_gymjot_cuff_DeviceCommand_start_video_tag: {
            if (g_videoState.active) {
                Serial.println("[VIDEO] Already streaming");
                sendStatusLabel("video-already-active", now);
                break;
            }
            if (!g_cameraReady) {
                Serial.println("[VIDEO] Camera not ready");
                sendStatusLabel("video-error-camera", now);
                break;
            }
            if (g_otaInProgress || g_photoCaptureInProgress) {
                Serial.println("[VIDEO] Blocked by other camera operation");
                sendStatusLabel("video-error-busy", now);
                break;
            }

            // Initialize video state
            g_videoState.active = true;
            g_videoState.sessionId = nextVideoSessionId();
            g_videoState.frameNumber = 0;
            g_videoState.fps = (cmd.command.start_video.fps > 0.1f && cmd.command.start_video.fps <= 30.0f)
                               ? cmd.command.start_video.fps : 5.0f;
            g_videoState.apriltagEnabled = cmd.command.start_video.enable_apriltag_detection;
            g_videoState.motionEnabled = cmd.command.start_video.enable_motion_detection;
            g_videoState.lastFrameMs = 0;

            Serial.println("[VIDEO] ===== VIDEO STARTED =====");
            Serial.print("[VIDEO] Session ID: ");
            Serial.println(g_videoState.sessionId);
            Serial.print("[VIDEO] FPS: ");
            Serial.println(g_videoState.fps);
            Serial.print("[VIDEO] AprilTag detection: ");
            Serial.println(g_videoState.apriltagEnabled ? "enabled" : "disabled");
            Serial.print("[VIDEO] Motion detection: ");
            Serial.println(g_videoState.motionEnabled ? "enabled" : "disabled");

            sendStatusLabel("video-started", now);
            break;
        }
        case com_gymjot_cuff_DeviceCommand_stop_video_tag: {
            if (!g_videoState.active) {
                Serial.println("[VIDEO] Not currently streaming");
                sendStatusLabel("video-not-active", now);
                break;
            }

            Serial.println("[VIDEO] ===== VIDEO STOPPED =====");
            Serial.print("[VIDEO] Total frames: ");
            Serial.println(g_videoState.frameNumber);

            // Clean up video state
            g_videoState.active = false;
            g_videoState.frameNumber = 0;
            if (g_videoState.previousFrame) {
                free(g_videoState.previousFrame);
                g_videoState.previousFrame = nullptr;
                g_videoState.previousFrameSize = 0;
            }

            sendStatusLabel("video-stopped", now);
            break;
        }
        case com_gymjot_cuff_DeviceCommand_snapshot_request_tag:
            updateSnapshotCharacteristic(now);
            sendSnapshotEvent(now);
            break;
        case com_gymjot_cuff_DeviceCommand_update_device_config_tag: {
            const auto& update = cmd.command.update_device_config;
            if (update.set_target_fps) {
                g_controller->setTargetFps(update.target_fps, now);
            }
            if (update.set_loiter_fps) {
                g_controller->setLoiterFps(update.loiter_fps, now);
            }
            if (update.set_min_travel_cm) {
                g_controller->setMinTravel(update.min_travel_cm, now);
            }
            if (update.set_max_rep_idle_ms) {
                g_controller->setMaxRepIdleMs(update.max_rep_idle_ms, now);
            }
            updateSnapshotCharacteristic(now);
            break;
        }
        case com_gymjot_cuff_DeviceCommand_ota_begin_tag: {
            g_otaInProgress = true;
            g_otaReceivedBytes = 0;
            g_otaTotalBytes = cmd.command.ota_begin.total_size;
            updateSnapshotCharacteristic(now);
            sendOtaStatus(com_gymjot_cuff_OtaPhase_OTA_PHASE_ERROR, "OTA not implemented", false, 0, g_otaTotalBytes);
            g_otaInProgress = false;
            updateSnapshotCharacteristic(now);
            break;
        }
        case com_gymjot_cuff_DeviceCommand_ota_chunk_tag:
            g_otaReceivedBytes += 0;
            sendOtaStatus(com_gymjot_cuff_OtaPhase_OTA_PHASE_ERROR, "OTA chunk ignored", false, g_otaReceivedBytes, g_otaTotalBytes);
            updateSnapshotCharacteristic(now);
            break;
        case com_gymjot_cuff_DeviceCommand_ota_complete_tag:
            sendOtaStatus(com_gymjot_cuff_OtaPhase_OTA_PHASE_ERROR, "OTA complete ignored", false, g_otaReceivedBytes, g_otaTotalBytes);
            g_otaInProgress = false;
            updateSnapshotCharacteristic(now);
            break;
        // NOTE: Uncomment after regenerating protobuf with clear_bonds command
        // case com_gymjot_cuff_DeviceCommand_clear_bonds_tag:
        //     if (cmd.command.clear_bonds.confirm) {
        //         Serial.println("=== CLEARING ALL BONDS (USER REQUEST) ===");
        //         int bondCount = NimBLEDevice::getNumBonds();
        //         NimBLEDevice::deleteAllBonds();
        //         Serial.print("Cleared ");
        //         Serial.print(bondCount);
        //         Serial.println(" bond(s)");
        //
        //         // Send confirmation event
        //         com_gymjot_cuff_DeviceEvent evt = com_gymjot_cuff_DeviceEvent_init_default;
        //         evt.timestamp_ms = now;
        //         evt.which_event = com_gymjot_cuff_DeviceEvent_status_tag;
        //         std::strncpy(evt.event.status.status_label, "bonds-cleared",
        //                      sizeof(evt.event.status.status_label) - 1);
        //         sendEvent(evt);
        //
        //         Serial.println("All bonds cleared - ready for fresh pairing");
        //     } else {
        //         Serial.println("Clear bonds cancelled (confirm=false)");
        //     }
        //     break;
        default:
            Serial.println("<- unknown command");
            break;
    }
}

void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println();
    Serial.println("========================================");
    Serial.println("    GymJot Cuff - Booting");
    Serial.println("========================================");
    Serial.print("Firmware version: ");
    Serial.println(kFirmwareVersion);
    Serial.println();

    // Print crash/reset reason
    esp_reset_reason_t resetReason = esp_reset_reason();
    Serial.print("Reset reason: ");
    switch(resetReason) {
        case ESP_RST_POWERON: Serial.println("Power-on reset"); break;
        case ESP_RST_SW: Serial.println("Software reset via esp_restart"); break;
        case ESP_RST_PANIC: Serial.println("!!! PANIC/EXCEPTION !!!"); break;
        case ESP_RST_INT_WDT: Serial.println("!!! INTERRUPT WATCHDOG !!!"); break;
        case ESP_RST_TASK_WDT: Serial.println("!!! TASK WATCHDOG !!!"); break;
        case ESP_RST_WDT: Serial.println("!!! OTHER WATCHDOG !!!"); break;
        case ESP_RST_DEEPSLEEP: Serial.println("Deep sleep wake"); break;
        case ESP_RST_BROWNOUT: Serial.println("!!! BROWNOUT !!!"); break;
        case ESP_RST_SDIO: Serial.println("SDIO reset"); break;
        default: Serial.println("Unknown"); break;
    }

    // Print free memory
    Serial.print("Free heap: ");
    Serial.print(ESP.getFreeHeap());
    Serial.println(" bytes");
    Serial.print("Free PSRAM: ");
    Serial.print(ESP.getFreePsram());
    Serial.println(" bytes");
    Serial.println();

#ifdef ENABLE_HEAP_SERIAL_LOGGING
    g_heapMonitor.enableSerialLogging(true);
#endif
    g_heapMonitor.update("boot", millis(), true);

    // Initialize watchdog timer (30 second timeout)
    Serial.println("Initializing watchdog timer (30s timeout)...");
    esp_task_wdt_init(30, true);
    esp_task_wdt_add(NULL);
    Serial.println("Watchdog enabled");

    g_identity = &gymjot::deviceIdentity();
    setupController();

    Serial.println();
    Serial.println("Initializing hardware...");

    // Initialize AprilTag detector before camera to reserve decode tables while memory is plentiful
    bool detectorReady = setupAprilTagDetector();
    if (detectorReady) {
        Serial.println("AprilTag detector: OK");
    } else {
        Serial.println("AprilTag detector: FAILED");
    }

    g_cameraReady = setupCamera();
    if (g_cameraReady) {
        Serial.println("Camera: OK");
    } else {
        Serial.println("Camera: FAILED");
    }

    Serial.println();
    setupBLE();

    uint64_t now = millis();
    sendBootStatus(now);
    sendSnapshotEvent(now);
    updateSnapshotCharacteristic(now);

    Serial.println();
    Serial.println("[BOOT] --------------------------------");
    Serial.println("[BOOT] System ready");
    Serial.println("[BOOT] Startup diagnostics");
    Serial.print("[BOOT] camera_ready=");
    Serial.println(g_cameraReady ? "true" : "false");
    Serial.print("[BOOT] detector_ready=");
    Serial.println(g_tagDetector ? "true" : "false");
    Serial.print("[BOOT] ble_advertising=");
    Serial.println("active");
    Serial.print("[BOOT] test_mode=");
    Serial.println(g_controller && g_controller->testMode() ? "enabled" : "disabled");
    Serial.println();
    Serial.println("[BOOT] AprilTag configuration");
    Serial.println("  family=tagCircle49h12");
    Serial.println("  tag_size_cm=5.5");
    Serial.println("  expected_ids=0-2400");
    Serial.println();
    Serial.println("[BOOT] Tips");
    if (!g_controller || !g_controller->testMode()) {
        Serial.println("  - Camera mode active");
        Serial.println("  - Use tagCircle49h12 family");
        Serial.println("  - Keep tag 30-100cm from camera");
        Serial.println("  - Provide even lighting");
        Serial.println("  - Watch for '[APRILTAG] Detection' logs");
    } else {
        Serial.println("  - Test mode active");
        Serial.println("  - No camera or tag required");
        Serial.println("  - Simulator will generate detections and reps");
    }
    Serial.println();
    Serial.println("[BOOT] Waiting for detections...");
    Serial.println("[BOOT] --------------------------------");
    Serial.println();

    esp_task_wdt_reset();
}

void loop() {
    uint64_t now = millis();

    // Process any deferred status notifications (from GATT callbacks)
    if (g_deferredStatus.pending) {
        Serial.print("[LOOP] Sending deferred status: ");
        Serial.println(g_deferredStatus.label);
        sendStatusLabel(g_deferredStatus.label, g_deferredStatus.timestamp);
        g_deferredStatus.pending = false;
    }

    // Heartbeat and status every 10 seconds
    static uint64_t lastHeartbeat = 0;
    if (now - lastHeartbeat > 10000) {
        g_heapMonitor.update("heartbeat", now, true);
        Serial.println("[STATUS] --------------------------------");
        Serial.print("[STATUS] Uptime_s=");
        Serial.println(now / 1000);
        Serial.print("[STATUS] camera_ready=");
        Serial.println(g_cameraReady ? "true" : "false");
        Serial.print("[STATUS] detector_ready=");
        Serial.println(g_tagDetector ? "true" : "false");
        Serial.print("[STATUS] test_mode=");
        Serial.println(g_controller && g_controller->testMode() ? "true" : "false");
        Serial.print("[STATUS] ble_client_connected=");
        Serial.println(g_clientConnected ? "true" : "false");
        if (g_controller) {
            Serial.print("[STATUS] mode=");
            switch (g_controller->mode()) {
                case gymjot::DeviceMode::Idle: Serial.println("Idle"); break;
                case gymjot::DeviceMode::AwaitingExercise: Serial.println("AwaitingExercise"); break;
                case gymjot::DeviceMode::Scanning: Serial.println("Scanning"); break;
                case gymjot::DeviceMode::Loiter: Serial.println("Loiter"); break;
            }
            Serial.print("[STATUS] target_fps=");
            Serial.println(g_controller->targetFps());
            Serial.print("[STATUS] rep_count=");
            Serial.println(g_controller->repTracker().count());
        }
        if (g_lastAprilTagDetectionMs > 0) {
            Serial.print("[STATUS] last_tag_age_ms=");
            Serial.println(now - g_lastAprilTagDetectionMs);
            Serial.print("[STATUS] last_tag_id=");
            Serial.println(g_lastAprilTagId);
            Serial.print("[STATUS] last_tag_distance_cm=");
            Serial.println(g_lastAprilTagDistanceCm);
            Serial.print("[STATUS] last_tag_margin=");
            Serial.println(g_lastAprilTagMargin);
        } else {
            Serial.println("[STATUS] last_tag_age_ms=never");
        }
        Serial.println("[STATUS] --------------------------------");
        lastHeartbeat = now;
    }


    // Reset watchdog timer
    static uint64_t lastWdtReset = 0;
    if (now - lastWdtReset > 5000) {
        esp_task_wdt_reset();
        lastWdtReset = now;
    }

    // DEFER connection parameter updates until after discovery and pairing
    // Wait for: encrypted + authenticated + first ATT request (discovery complete)
    if (g_clientConnected && !g_connectionOptimized && g_server && g_firstAttRequestTimestamp > 0) {
        auto peers = g_server->getPeerDevices();
        if (!peers.empty()) {
            auto connInfo = g_server->getPeerInfo(peers[0]);
            if (connInfo.isEncrypted() && connInfo.isAuthenticated()) {
                // Additional delay to ensure discovery is complete
                if (now - g_firstAttRequestTimestamp > 500) {
                    g_connectionOptimized = true;

                    Serial.println("=== PAIRING & DISCOVERY COMPLETE ===");
                    Serial.println("Requesting optimized connection parameters...");

                    // Safe connection parameters per BLE best practices:
                    // conn_interval = 30-50ms, slave_latency = 0-4, supervision_timeout = 5-6s
                    // Rule: timeout_ms >= 2 * interval_ms * (1 + latency) * 3
                    // Example: 5000 >= 2 * 40 * (1 + 2) * 3 = 720 (ok)
                    g_server->updateConnParams(connInfo.getConnHandle(),
                        24,   // min_interval (24 * 1.25ms = 30ms)
                        40,   // max_interval (40 * 1.25ms = 50ms)
                        2,    // slave_latency (allows skipping 2 events for power saving)
                        500   // supervision_timeout (500 * 10ms = 5000ms = 5s)
                    );

                    Serial.println("Requested params: interval=30-50ms, latency=2, timeout=5s");
                    Serial.println("(Central may choose different values)");

                    // Log final telemetry summary
                    Serial.println();
                    Serial.println("=== CONNECTION TELEMETRY SUMMARY ===");
                    Serial.print("MTU negotiation: +");
                    Serial.print(g_mtuNegotiatedTimestamp - g_connectTimestamp);
                    Serial.print("ms (");
                    Serial.print(g_currentMtu);
                    Serial.println(" bytes)");
                    Serial.print("First ATT request: +");
                    Serial.print(g_firstAttRequestTimestamp - g_connectTimestamp);
                    Serial.println("ms");
                    Serial.print("Connection optimization: +");
                    Serial.print(now - g_connectTimestamp);
                    Serial.println("ms");

                    // Send pairing success event
                    com_gymjot_cuff_DeviceEvent evt = com_gymjot_cuff_DeviceEvent_init_default;
                    evt.timestamp_ms = now;
                    evt.which_event = com_gymjot_cuff_DeviceEvent_status_tag;
                    std::strncpy(evt.event.status.status_label, "pairing-success",
                                 sizeof(evt.event.status.status_label) - 1);
                    sendEvent(evt);
                }
            }
        }
    }

    // Reset optimization flag on disconnect
    if (!g_clientConnected && g_connectionOptimized) {
        g_connectionOptimized = false;
    }

    if (g_controller) {
        g_controller->maintainTestMode(now);
    }

    // Video streaming (takes priority over normal AprilTag detection)
    if (g_videoState.active && !g_photoCaptureInProgress) {
        float videoInterval = 1000.0f / g_videoState.fps;
        bool videoFrameReady = (now - g_videoState.lastFrameMs) >= static_cast<uint64_t>(videoInterval);

        if (videoFrameReady) {
            g_videoState.lastFrameMs = now;
            captureAndStreamVideoFrame(now);
        }
    }
    // Skip AprilTag detection during photo capture or video streaming to avoid camera resource conflicts
    else if (!g_photoCaptureInProgress) {
        static uint64_t lastFrameMs = 0;
        static uint64_t frameCount = 0;
        float interval = g_controller ? g_controller->frameIntervalMs() : 125.0f;
        bool frameReady = (now - lastFrameMs) >= static_cast<uint64_t>(interval);

        if (frameReady) {
            lastFrameMs = now;
            frameCount++;

            // Frame capture debug (every 50 frames)
            if (frameCount % 50 == 0) {
                Serial.print("[FRAME] count=");
                Serial.print(frameCount);
                Serial.print(" interval_ms=");
                Serial.println(interval);
            }

            esp_task_wdt_reset();
            g_heapMonitor.update("apriltag-loop", now);

            AprilTagDetection detection{};
            bool hasDetection = false;

            if (!g_heapMonitor.shouldThrottle(now)) {
                if (g_controller && g_controller->testMode()) {
                    uint32_t id = g_controller->session().active ? g_controller->session().tagId : TEST_EXERCISE_ID;
                    hasDetection = g_controller->testSimulator().generate(id, detection);

                    if (frameCount % 50 == 0) {
                        Serial.println("[FRAME] using test mode simulator");
                    }
                } else {
                    hasDetection = captureAprilTag(detection);
                }

                if (hasDetection) {
                    if (!g_controller || !g_controller->testMode()) {
                        ++g_aprilTagDetectionCount;
                        if (kDetectionsBeforeAutoReset &&
                            (g_aprilTagDetectionCount % kDetectionsBeforeAutoReset) == 0) {
                            g_resetScheduler.request("apriltag-rotation", now);
                        }
                    }
                    g_heapMonitor.update("apriltag-post", now);
                }

                if (hasDetection && g_controller) {
                    g_controller->handleDetection(detection, now);
                }
            }
        }
    }

    static uint64_t lastSnapshot = 0;
    if (now - lastSnapshot > 2000) {
        updateSnapshotCharacteristic(now);
        lastSnapshot = now;
    }

    handlePendingPhotoRequest(now);
    g_resetScheduler.service(now, g_photoCaptureInProgress || g_videoState.active);

    delay(5);
}




