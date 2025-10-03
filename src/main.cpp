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
using gymjot::StationPayload;

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
        g_lastConnectionTime = millis();

        Serial.println("=== BLE CLIENT CONNECTED ===");
        Serial.print("Client address: ");
        Serial.println(connInfo.getAddress().toString().c_str());
        Serial.print("Connection ID: ");
        Serial.println(connInfo.getConnHandle());
        Serial.print("MTU: ");
        Serial.println(pServer->getPeerMTU(connInfo.getConnHandle()));

        // Request connection parameter update for better reliability
        pServer->updateConnParams(connInfo.getConnHandle(), 12, 24, 0, 400);

        // Send connection event (delayed to allow MTU negotiation)
        delay(100);
        com_gymjot_cuff_DeviceEvent evt = com_gymjot_cuff_DeviceEvent_init_default;
        evt.timestamp_ms = millis();
        evt.which_event = com_gymjot_cuff_DeviceEvent_status_tag;
        std::strncpy(evt.event.status.status_label, "ble-connected", sizeof(evt.event.status.status_label) - 1);
        sendEvent(evt);
    }

    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
        g_clientConnected = false;

        Serial.println("=== BLE CLIENT DISCONNECTED ===");
        Serial.print("Connection duration: ");
        Serial.print((millis() - g_lastConnectionTime) / 1000);
        Serial.println(" seconds");
        Serial.print("Reason code: ");
        Serial.print(reason);
        Serial.print(" - ");

        // Decode disconnect reason
        switch(reason) {
            case 0x08: Serial.println("Connection timeout"); break;
            case 0x13: Serial.println("Remote user terminated"); break;
            case 0x16: Serial.println("Connection terminated by local host"); break;
            case 0x3D: Serial.println("Connection failed to establish"); break;
            case 0x3E: Serial.println("LMP response timeout"); break;
            case 0x22: Serial.println("LMP error / Connection terminated"); break;
            default: Serial.println("Other/Unknown"); break;
        }

        Serial.print("Restarting advertising...");
        if (NimBLEDevice::startAdvertising()) {
            Serial.println("OK");
        } else {
            Serial.println("FAILED - attempting restart");
            delay(100);
            NimBLEDevice::startAdvertising();
        }

        // Send disconnection event
        com_gymjot_cuff_DeviceEvent evt = com_gymjot_cuff_DeviceEvent_init_default;
        evt.timestamp_ms = millis();
        evt.which_event = com_gymjot_cuff_DeviceEvent_status_tag;
        char msg[64];
        snprintf(msg, sizeof(msg), "ble-disconnected-0x%02X", reason);
        std::strncpy(evt.event.status.status_label, msg, sizeof(evt.event.status.status_label) - 1);
        sendEvent(evt);
    }

    void onMTUChange(uint16_t MTU, NimBLEConnInfo& connInfo) override {
        Serial.print("MTU negotiated: ");
        Serial.println(MTU);
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
        return false;
    }

    // Don't try to send if no client is connected
    if (!g_clientConnected) {
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

    g_commandChar = service->createCharacteristic(CHAR_RX_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC);
    g_commandChar->setCallbacks(new RxCallback());

    g_tx = service->createCharacteristic(CHAR_TX_UUID,
        NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC);

    g_snapshotChar = service->createCharacteristic(CHAR_SNAPSHOT_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::NOTIFY);
    g_snapshotChar->setCallbacks(new SnapshotCallback());

    g_infoChar = service->createCharacteristic(CHAR_INFO_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC);
    g_infoChar->setCallbacks(new InfoCallback());

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
            updateSnapshotCharacteristic(now);
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

    // Initialize watchdog timer (30 second timeout)
    Serial.println("Initializing watchdog timer (30s timeout)...");
    esp_task_wdt_init(30, true);
    esp_task_wdt_add(NULL);
    Serial.println("Watchdog enabled");

    g_identity = &gymjot::deviceIdentity();
    setupController();

    Serial.println();
    Serial.println("Initializing hardware...");
    g_cameraReady = setupCamera();
    if (g_cameraReady) {
        Serial.println("Camera: OK");
    } else {
        Serial.println("Camera: FAILED");
    }

    if (setupAprilTagDetector()) {
        Serial.println("AprilTag detector: OK");
    } else {
        Serial.println("AprilTag detector: FAILED");
    }

    Serial.println();
    setupBLE();

    uint64_t now = millis();
    sendBootStatus(now);
    sendSnapshotEvent(now);
    updateSnapshotCharacteristic(now);

    Serial.println();
    Serial.println("========================================");
    Serial.println("    Boot Complete - System Ready");
    Serial.println("========================================");
    Serial.println();

    esp_task_wdt_reset();
}

void loop() {
    uint64_t now = millis();

    // Reset watchdog timer
    static uint64_t lastWdtReset = 0;
    if (now - lastWdtReset > 5000) {
        esp_task_wdt_reset();
        lastWdtReset = now;
    }

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

    static uint64_t lastSnapshot = 0;
    if (now - lastSnapshot > 2000) {
        updateSnapshotCharacteristic(now);
        lastSnapshot = now;
    }

    delay(5);
}


