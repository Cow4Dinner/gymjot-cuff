#include <Arduino.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
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

static const size_t BLE_JSON_CAPACITY = 512;

static void processCommand(const std::string& cmd);

static void sendMessage(const std::string& msg) {
    if (g_tx) {

        g_tx->setValue(msg);
        g_tx->notify();
    }
    Serial.print("-> ");
    Serial.println(msg.c_str());
}

class RxCallback : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
        std::string val = characteristic->getValue();
        if (!val.empty()) {
            processCommand(val);
        }
    }
};

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
    cfg.testStationMetadata = TEST_STATION_METADATA;

    g_controller = std::make_unique<CuffController>(cfg, sendMessage);
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
    StaticJsonDocument<128> doc;
    doc["type"] = "status";
    doc["status"] = "boot";
    doc["timestamp"] = nowMs;
    if (g_controller && g_controller->testMode()) {
        doc["testMode"] = true;
    }
    std::string msg;
    serializeJson(doc, msg);
    sendMessage(msg);
}

void processCommand(const std::string& cmd) {
    if (!g_controller) {
        return;
    }

    StaticJsonDocument<BLE_JSON_CAPACITY> doc;
    DeserializationError err = deserializeJson(doc, cmd);
    uint64_t now = millis();

    if (err) {
        if (cmd.find("test") != std::string::npos) {
            bool enable = cmd.find("true") != std::string::npos;
            g_controller->setTestMode(enable, now);
        }
        return;
    }

    std::string type = doc["type"].as<std::string>();
    if (type == "test") {
        bool enabled = doc["enabled"].isNull() ? doc["value"].as<bool>() : doc["enabled"].as<bool>();
        g_controller->setTestMode(enabled, now);
    } else if (type == "fps") {
        float fps = doc["fps"].isNull() ? g_controller->targetFps() : doc["fps"].as<float>();
        g_controller->setTargetFps(fps, now);
    } else if (type == "station") {
        StationPayload payload;
        payload.id = doc["id"].as<uint32_t>();
        payload.name = doc["name"].as<std::string>();
        if (!doc["metadata"].isNull()) {
            serializeJson(doc["metadata"], payload.metadataJson);
        }
        if (doc.containsKey("minTravelCm")) {
            payload.minTravelCm = doc["minTravelCm"].as<float>();
        }
        if (doc.containsKey("fps")) {
            payload.fps = doc["fps"].as<float>();
        }
        g_controller->handleStationPayload(payload, now);
    } else if (type == "resetReps") {
        g_controller->resetReps(now);
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


