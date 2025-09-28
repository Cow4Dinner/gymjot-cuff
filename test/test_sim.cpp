#include <unity.h>

#include "CuffController.h"
#include "proto/cuff.pb.h"

#include <string>
#include <vector>

extern "C" void setUp(void) {}
extern "C" void tearDown(void) {}

using gymjot::AprilTagDetection;
using gymjot::ControllerConfig;
using gymjot::CuffController;
using gymjot::MetadataList;
using gymjot::StationPayload;

namespace {
struct Sink {
    std::vector<com_gymjot_cuff_DeviceEvent> events;
    void operator()(const com_gymjot_cuff_DeviceEvent& evt) {
        events.push_back(evt);
    }
    void clear() { events.clear(); }
};
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
    cfg.testStationMetadata = { {"exercise", "Row"} };

    CuffController controller(cfg, [&](const com_gymjot_cuff_DeviceEvent& evt) { sink(evt); });

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

    for (const auto& evt : sink.events) {
        switch (evt.which_event) {
            case com_gymjot_cuff_DeviceEvent_scan_tag:
                sawScan = true;
                TEST_ASSERT_EQUAL_UINT32(cfg.testStationId, evt.event.scan.tag_id);
                break;
            case com_gymjot_cuff_DeviceEvent_rep_tag:
                sawRep = true;
                reps = evt.event.rep.rep_count;
                break;
            default:
                break;
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
    cfg.testStationMetadata = {};

    CuffController controller(cfg, [&](const com_gymjot_cuff_DeviceEvent& evt) { sink(evt); });

    uint64_t now = 0;
    AprilTagDetection detection{1234, 80.0f};
    controller.handleDetection(detection, now);

    StationPayload payload;
    payload.id = 1234;
    payload.name = "Bench";
    payload.metadata = { {"tempo", "slow"} };
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
    for (const auto& evt : sink.events) {
        if (evt.which_event == com_gymjot_cuff_DeviceEvent_scan_tag) {
            sawScan = true;
            TEST_ASSERT_TRUE(evt.event.scan.has_station_name);
            TEST_ASSERT_EQUAL_STRING("Bench", evt.event.scan.station_name);
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
