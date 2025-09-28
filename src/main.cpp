#include <Arduino.h>
#include <NimBLEDevice.h>
#include <pb_decode.h>
#include <pb_encode.h>

#include "proto/cuff.pb.h"
#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <cmath>

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
using gymjot::StationPayload;

static NimBLEServer* g_server = nullptr;
static NimBLECharacteristic* g_tx = nullptr;
static std::unique_ptr<CuffController> g_controller;
static bool g_cameraReady = false;
static apriltag_family_t* g_tagFamily = nullptr;
static apriltag_detector_t* g_tagDetector = nullptr;

static constexpr size_t kProtoBufferSize = 512;
static constexpr size_t kLengthPrefixBytes = 2;

static com_gymjot_cuff_DeviceMode toProtoMode(gymjot::DeviceMode mode) {
    switch (mode) {
        case gymjot::DeviceMode::Idle:
            return com_gymjot_cuff_DeviceMode_DEVICE_MODE_IDLE;
        case gymjot::DeviceMode::AwaitingStation:
            return com_gymjot_cuff_DeviceMode_DEVICE_MODE_AWAITING_STATION;
        case gymjot::DeviceMode::Scanning:
            return com_gymjot_cuff_DeviceMode_DEVICE_MODE_SCANNING;
        case gymjot::DeviceMode::Loiter:
            return com_gymjot_cuff_DeviceMode_DEVICE_MODE_LOITER;
    }
    return com_gymjot_cuff_DeviceMode_DEVICE_MODE_IDLE;
}

static gymjot::MetadataList metadataFromProto(const com_gymjot_cuff_StationMetadata& metadata) {
    gymjot::MetadataList result;
    result.reserve(metadata.entries_count);
    for (pb_size_t i = 0; i < metadata.entries_count; ++i) {
        const auto& entry = metadata.entries[i];
        result.push_back({entry.key, entry.value});
    }
    return result;
}

static bool sendEvent(const com_gymjot_cuff_DeviceEvent& event);
static void processCommand(const uint8_t* data, size_t len);

static void logPacket(size_t len) {
    Serial.print("-> [");
    Serial.print(len);
    Serial.println(" bytes]");
}

class RxCallback : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
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

static bool sendEvent(const com_gymjot_cuff_DeviceEvent& event) {
    if (!g_tx) {
        return false;
    }

    std::array<uint8_t, kLengthPrefixBytes + kProtoBufferSize> buffer{};
    pb_ostream_t stream = pb_ostream_from_buffer(buffer.data() + kLengthPrefixBytes, kProtoBufferSize);
    if (!pb_encode(&stream, com_gymjot_cuff_DeviceEvent_fields, &event)) {
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
    const size_t totalLen = payloadLen + kLengthPrefixBytes;

    g_tx->setValue(buffer.data(), totalLen);
    if (!g_tx->notify()) {
        Serial.println("notify failed");
        return false;
    }

    logPacket(totalLen);
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
    cfg.testStationId = TEST_STATION_ID;
    cfg.testStationName = TEST_STATION_NAME;
    cfg.testStationMetadata = defaultTestStationMetadata();

    g_controller = std::make_unique<CuffController>(cfg, sendEvent);
}

static bool setupCamera() {
    camera_config_t config;
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
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

    if (esp_camera_init(&config) != ESP_OK) {
        Serial.println("Camera init failed");
        return false;
    }

    sensor_t* sensor = esp_camera_sensor_get();
    if (sensor) {
        sensor->set_framesize(sensor, FRAMESIZE_QQVGA);
    }

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
    NimBLEDevice::init("ESP32CamStation");
    g_server = NimBLEDevice::createServer();
    NimBLEService* service = g_server->createService(SERVICE_UUID);

    NimBLECharacteristic* rx = service->createCharacteristic(
        CHAR_RX_UUID, NIMBLE_PROPERTY::WRITE);
    g_tx = service->createCharacteristic(
        CHAR_TX_UUID, NIMBLE_PROPERTY::NOTIFY);
    rx->setCallbacks(new RxCallback());

    service->start();
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(SERVICE_UUID);
    NimBLEDevice::startAdvertising();
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
        return false;
    }

    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("Failed to get frame");
        return false;
    }

    image_u8_t image = { static_cast<int32_t>(fb->width), static_cast<int32_t>(fb->height), static_cast<int32_t>(fb->width), fb->buf };
    zarray_t* detections = apriltag_detector_detect(g_tagDetector, &image);

    apriltag_detection_t* best = nullptr;
    double bestMargin = 0.0;

    for (int i = 0; i < zarray_size(detections); ++i) {
        apriltag_detection_t* det = nullptr;
        zarray_get(detections, i, &det);
        if (!det) {
            continue;
        }
        if (det->family != g_tagFamily) {
            continue;
        }
        if (!best || det->decision_margin > bestMargin) {
            best = det;
            bestMargin = det->decision_margin;
        }
    }

    bool found = false;
    if (best) {
        detection.tagId = static_cast<uint32_t>(best->id);
        detection.distanceCm = computeDetectionDistance(best);
        found = true;
    }

    apriltag_detections_destroy(detections);
    esp_camera_fb_return(fb);
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
            break;
        case com_gymjot_cuff_DeviceCommand_station_update_tag: {
            const auto& update = cmd.command.station_update;
            StationPayload payload;
            payload.id = update.station_id;
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
            g_controller->handleStationPayload(payload, now);
            break;
        }
        case com_gymjot_cuff_DeviceCommand_reset_reps_tag:
            g_controller->resetReps(now);
            break;
        default:
            Serial.println("<- unknown command");
            break;
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println("Booting");

    g_cameraReady = setupCamera();
    setupAprilTagDetector();
    setupBLE();
    setupController();

    uint64_t now = millis();
    sendBootStatus(now);
}

void loop() {
    uint64_t now = millis();

    if (g_controller) {
        g_controller->maintainTestMode(now);
    }

    static uint64_t lastFrameMs = 0;
    float interval = g_controller ? g_controller->frameIntervalMs() : 125.0f;
    bool frameReady = (now - lastFrameMs) >= static_cast<uint64_t>(interval);

    if (frameReady) {
        lastFrameMs = now;
        AprilTagDetection detection;
        bool hasDetection = false;

        if (g_controller && g_controller->testMode()) {
            uint32_t id = g_controller->session().active ? g_controller->session().tagId : TEST_STATION_ID;
            hasDetection = g_controller->testSimulator().generate(id, detection);
        } else {
            hasDetection = captureAprilTag(detection);
        }

        if (hasDetection && g_controller) {
            g_controller->handleDetection(detection, now);
        }
    }

    if (g_controller) {
        g_controller->evaluateTimeouts(now);
    }

    delay(5);
}


