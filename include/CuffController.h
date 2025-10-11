#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <cmath>
#include <limits>

#include "MetadataTypes.h"
#include "proto/cuff.pb.h"

namespace gymjot {

enum class DeviceMode { Idle, AwaitingExercise, Scanning, Loiter };

struct AprilTagDetection {
    uint32_t tagId = 0;
    float distanceCm = 0.0f;
};

struct ExerciseSession {
    bool active = false;
    bool metadataReady = false;
    bool requestSent = false;
    uint32_t tagId = 0;
    std::string name;
    MetadataList metadata;
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
    float minTravelCm() const { return minTravelCm_; }
    uint64_t maxIdleMs() const { return maxRepIdleMs_; }
    Phase phase() const { return phase_; }
    float bottomRef() const { return bottomRef_; }
    float peakDistance() const { return peakDistance_; }

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
    uint32_t testExerciseId = 4242;
    std::string testExerciseName = "Demo Exercise";
    MetadataList testExerciseMetadata;
};

struct ExercisePayload {
    uint32_t id = 0;
    std::string name;
    MetadataList metadata;
    std::optional<float> minTravelCm;
    std::optional<float> fps;
};

class CuffController {
public:
    using SendCallback = std::function<void(const com_gymjot_cuff_DeviceEvent&)>;

    CuffController(const ControllerConfig& config, SendCallback sendFn);

    void setTestMode(bool enabled, uint64_t nowMs);
    bool testMode() const { return testMode_; }

    void setTargetFps(float fps, uint64_t nowMs);
    float targetFps() const { return targetFps_; }

    void setLoiterFps(float fps, uint64_t nowMs);
    float loiterFps() const { return config_.loiterFps; }

    void setMaxRepIdleMs(uint32_t value, uint64_t nowMs);
    uint32_t maxRepIdleMs() const { return config_.maxRepIdleMs; }

    void setMinTravel(float cm, uint64_t nowMs);
    float minTravelCm() const { return repTracker_.minTravelCm(); }

    void resetReps(uint64_t nowMs);

    DeviceMode mode() const { return deviceMode_; }
    float frameIntervalMs() const;

    ExerciseSession& session() { return session_; }
    const ExerciseSession& session() const { return session_; }
    RepTracker& repTracker() { return repTracker_; }
    const RepTracker& repTracker() const { return repTracker_; }
    TestModeSimulator& testSimulator() { return testSimulator_; }

    void handleDetection(const AprilTagDetection& detection, uint64_t nowMs);
    void evaluateTimeouts(uint64_t nowMs);
    void maintainTestMode(uint64_t nowMs);
    void startTestSession(uint64_t nowMs);
    void applyExerciseMetadata(uint32_t tagId, const std::string& name, const MetadataList& metadata, uint64_t nowMs);
    void handleExercisePayload(const ExercisePayload& payload, uint64_t nowMs);

    void setSendCallback(SendCallback cb) { send_ = std::move(cb); }

private:
    void notifyStatus(const std::string& status, uint64_t nowMs);
    void sendTagAnnouncement(uint32_t tagId, uint64_t nowMs, bool fromTest);
    void sendExerciseRequest(uint32_t tagId, uint64_t nowMs);
    void sendExerciseBroadcast(uint32_t id, const std::string& name, const MetadataList& metadata, uint64_t nowMs, bool fromTest);
    void sendScan(const AprilTagDetection& detection, uint64_t nowMs);
    void sendRep(uint64_t nowMs);
    void enterLoiter(uint64_t nowMs);
    void exitLoiter(uint64_t nowMs);

    ControllerConfig config_;
    SendCallback send_;

    bool testMode_ = true;
    float targetFps_ = 8.0f;
    DeviceMode deviceMode_ = DeviceMode::Idle;
    ExerciseSession session_;
    RepTracker repTracker_;
    TestModeSimulator testSimulator_;
};

}  // namespace gymjot

