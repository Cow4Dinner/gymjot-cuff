#include <ArduinoJson.h>
#include <unity.h>

#include "CuffController.h"

#include <vector>
#include <string>

extern "C" void setUp(void) {}
extern "C" void tearDown(void) {}

using gymjot::AprilTagDetection;
using gymjot::ControllerConfig;
using gymjot::CuffController;
using gymjot::StationPayload;

namespace {
struct Sink {
    std::vector<std::string> messages;
    void operator()(const std::string& msg) {
        messages.push_back(msg);
    }
    void clear() { messages.clear(); }
};

StaticJsonDocument<512> parseJson(const std::string& payload) {
    StaticJsonDocument<512> doc;
    auto err = deserializeJson(doc, payload);
    TEST_ASSERT_TRUE_MESSAGE(err == DeserializationError::Ok, payload.c_str());
    return doc;
}
}

static void test_testmode_generates_messages() {
    Sink sink;
    ControllerConfig cfg;
    cfg.defaultTestMode = true;
    cfg.defaultFps = 4.0f;
    cfg.loiterFps = 0.5f;
    cfg.tagLostMs = 2000;
    cfg.defaultMinTravelCm = 8.0f;
    cfg.maxRepIdleMs = 1000;
    cfg.testStationId = 4242;
    cfg.testStationName = "Sim Station";
    cfg.testStationMetadata = "{\"exercise\":\"Row\"}";

    CuffController controller(cfg, [&](const std::string& msg) { sink(msg); });

    uint64_t now = 0;
    controller.maintainTestMode(now);
    TEST_ASSERT_TRUE(controller.session().active);
    sink.clear();

    for (int i = 0; i < 80; ++i) {
        AprilTagDetection detection;
        controller.testSimulator().generate(cfg.testStationId, detection);
        controller.handleDetection(detection, now);
        now += 200;
    }

    bool sawScan = false;
    bool sawRep = false;
    uint32_t reps = 0;

    for (const auto& msg : sink.messages) {
        auto doc = parseJson(msg);
        std::string type = doc["type"].as<std::string>();
        if (type == "scan") {
            sawScan = true;
            TEST_ASSERT_EQUAL_UINT(cfg.testStationId, doc["tagId"].as<uint32_t>());
        }
        if (type == "rep") {
            sawRep = true;
            reps = doc["count"].as<uint32_t>();
        }
    }

    TEST_ASSERT_TRUE(sawScan);
    TEST_ASSERT_TRUE(sawRep);
    TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(0, reps, "Expected at least one repetition");
    TEST_ASSERT_EQUAL_UINT(reps, controller.repTracker().count());
}

static void test_station_payload_updates_config() {
    Sink sink;
    ControllerConfig cfg;
    cfg.defaultTestMode = false;
    cfg.defaultFps = 6.0f;
    cfg.loiterFps = 1.0f;

    CuffController controller(cfg, [&](const std::string& msg) { sink(msg); });

    uint64_t now = 0;
    AprilTagDetection detection{1234, 80.0f};
    controller.handleDetection(detection, now);

    StationPayload payload;
    payload.id = 1234;
    payload.name = "Bench";
    payload.metadataJson = "{}";
    payload.minTravelCm = 5.0f;
    payload.fps = 5.0f;

    controller.handleStationPayload(payload, now);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 5.0f, controller.targetFps());

    sink.clear();
    for (int i = 0; i < 12; ++i) {
        controller.handleDetection(detection, now);
        now += 150;
    }

    bool sawScan = false;
    for (const auto& msg : sink.messages) {
        auto doc = parseJson(msg);
        if (doc["type"].as<std::string>() == "scan") {
            sawScan = true;
            TEST_ASSERT_EQUAL_STRING("Bench", doc["stationName"].as<const char*>());
        }
    }
    TEST_ASSERT_TRUE(sawScan);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_testmode_generates_messages);
    RUN_TEST(test_station_payload_updates_config);
    return UNITY_END();
}




