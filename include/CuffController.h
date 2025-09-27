#pragma once

#include <ArduinoJson.h>
#include <cstdint>
#include <functional>
#include <optional>
#include <cmath>
#include <limits>
#include <string>

namespace gymjot {

enum class DeviceMode { Idle, AwaitingStation, Scanning, Loiter };

struct AprilTagDetection {
    uint32_t tagId = 0;
    float distanceCm = 0.0f;
};

struct StationSession {
    bool active = false;
    bool metadataReady = false;
    bool requestSent = false;
    uint32_t tagId = 0;
    std::string name;
    std::string metadataJson;
    uint64_t lastSeenMs = 0;
    uint64_t lastRequestMs = 0;

    void reset();
};

class RepTracker {
public:
    enum class Phase { WaitingBottom, Ascending, Descending };

    void reset(uint32_t startCount = 0);
    uint32_t count() const;
    void setMinTravel(float travelCm);
    void setMaxIdleMs(uint64_t maxIdleMs);
    bool update(float distanceCm, uint64_t nowMs);

private:
    Phase phase_ = Phase::WaitingBottom;
    uint32_t reps_ = 0;
    float minTravelCm_ = 12.0f;
    float bottomRef_ = std::numeric_limits<float>::quiet_NaN();
    float peakDistance_ = std::numeric_limits<float>::quiet_NaN();
    float descentTarget_ = std::numeric_limits<float>::quiet_NaN();
    float lastDistance_ = std::numeric_limits<float>::quiet_NaN();
    uint64_t lastMovementMs_ = 0;
    uint64_t maxRepIdleMs_ = 5000;
};

class TestModeSimulator {
public:
    void reset();
    bool generate(uint32_t tagId, AprilTagDetection& detection);
    bool active() const { return active_; }

private:
    bool active_ = false;
    float distanceCm_ = 85.0f;
    bool movingUp_ = true;
    const float bottom_ = 85.0f;
    const float top_ = 35.0f;
    const float step_ = 5.0f;
};

struct ControllerConfig {
    float defaultFps = 8.0f;
    bool defaultTestMode = true;
    float loiterFps = 0.3333f;
    uint32_t tagLostMs = 10000;
    float defaultMinTravelCm = 12.0f;
    uint32_t maxRepIdleMs = 5000;
    uint32_t testStationId = 4242;
    std::string testStationName = "Demo Station";
    std::string testStationMetadata = "{\"exercise\":\"Lat Pulldown\",\"muscleGroup\":\"Back\",\"intensity\":\"moderate\"}";
};

struct StationPayload {
    uint32_t id = 0;
    std::string name;
    std::string metadataJson;
    std::optional<float> minTravelCm;
    std::optional<float> fps;
};

class CuffController {
public:
    using SendCallback = std::function<void(const std::string&)>;

    CuffController(const ControllerConfig& config, SendCallback sendFn);

    void setTestMode(bool enabled, uint64_t nowMs);
    bool testMode() const { return testMode_; }

    void setTargetFps(float fps, uint64_t nowMs);
    float targetFps() const { return targetFps_; }

    void resetReps(uint64_t nowMs);

    DeviceMode mode() const { return deviceMode_; }
    float frameIntervalMs() const;

    StationSession& session() { return session_; }
    RepTracker& repTracker() { return repTracker_; }
    TestModeSimulator& testSimulator() { return testSimulator_; }

    void handleDetection(const AprilTagDetection& detection, uint64_t nowMs);
    void evaluateTimeouts(uint64_t nowMs);
    void maintainTestMode(uint64_t nowMs);
    void startTestSession(uint64_t nowMs);
    void applyStationMetadata(uint32_t tagId, const std::string& name, const std::string& metadataJson, uint64_t nowMs);
    void handleStationPayload(const StationPayload& payload, uint64_t nowMs);

    void setSendCallback(SendCallback cb) { send_ = std::move(cb); }

private:
    void notifyStatus(const std::string& status, uint64_t nowMs);
    void sendJsonDoc(const JsonDocument& doc);
    void sendTagAnnouncement(uint32_t tagId, uint64_t nowMs, bool fromTest);
    void sendStationRequest(uint32_t tagId, uint64_t nowMs);
    void sendStationBroadcast(uint32_t id, const std::string& name, const std::string& metadataJson, uint64_t nowMs, const char* origin);
    void sendScan(const AprilTagDetection& detection, uint64_t nowMs);
    void sendRep(uint64_t nowMs);
    void enterLoiter(uint64_t nowMs);
    void exitLoiter(uint64_t nowMs);

    ControllerConfig config_;
    SendCallback send_;

    bool testMode_ = true;
    float targetFps_ = 8.0f;
    DeviceMode deviceMode_ = DeviceMode::Idle;
    StationSession session_;
    RepTracker repTracker_;
    TestModeSimulator testSimulator_;
};

}  // namespace gymjot

