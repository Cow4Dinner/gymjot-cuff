#include "CuffController.h"

#include "Config.h"
#include "PersistentConfig.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace gymjot {

namespace {

constexpr size_t kMaxMetadataEntries = 10;

void printSanitizedLine(const char* prefix, const std::string& value) {
#ifdef ARDUINO
    Serial.print(prefix);
    for (unsigned char c : value) {
        if (c >= 32 && c <= 126) {
            Serial.print(static_cast<char>(c));
        } else {
            Serial.print('?');
        }
    }
    Serial.println();
#else
    (void)prefix;
    (void)value;
#endif
}

com_gymjot_cuff_DeviceMode toProto(DeviceMode mode) {
    switch (mode) {
        case DeviceMode::Idle:
            return com_gymjot_cuff_DeviceMode_DEVICE_MODE_IDLE;
        case DeviceMode::AwaitingExercise:
            return com_gymjot_cuff_DeviceMode_DEVICE_MODE_AWAITING_EXERCISE;
        case DeviceMode::Scanning:
            return com_gymjot_cuff_DeviceMode_DEVICE_MODE_SCANNING;
        case DeviceMode::Loiter:
            return com_gymjot_cuff_DeviceMode_DEVICE_MODE_LOITER;
    }
    return com_gymjot_cuff_DeviceMode_DEVICE_MODE_IDLE;
}

void copyString(const std::string& source, char* dest, size_t capacity) {
    if (capacity == 0) {
        return;
    }
    std::memset(dest, 0, capacity);
    std::strncpy(dest, source.c_str(), capacity - 1);
}

void populateMetadata(const MetadataList& source, com_gymjot_cuff_ExerciseMetadata& target) {
    std::memset(&target, 0, sizeof(target));
    target.entries_count = static_cast<pb_size_t>(std::min<size_t>(source.size(), kMaxMetadataEntries));
    for (pb_size_t i = 0; i < target.entries_count; ++i) {
        const auto& entry = source[i];
        copyString(entry.key, target.entries[i].key, sizeof(target.entries[i].key));
        copyString(entry.value, target.entries[i].value, sizeof(target.entries[i].value));
    }
}

}  // namespace

void ExerciseSession::reset() {
    active = false;
    metadataReady = false;
    requestSent = false;
    tagId = 0;
    name.clear();
    metadata.clear();
    lastSeenMs = 0;
    lastRequestMs = 0;
}

void RepTracker::reset(uint32_t startCount) {
    reps_ = startCount;
    phase_ = Phase::WaitingBottom;
    bottomRef_ = std::numeric_limits<float>::quiet_NaN();
    peakDistance_ = std::numeric_limits<float>::quiet_NaN();
    descentTarget_ = std::numeric_limits<float>::quiet_NaN();
    lastDistance_ = std::numeric_limits<float>::quiet_NaN();
    lastMovementMs_ = 0;
}

uint32_t RepTracker::count() const {
    return reps_;
}

void RepTracker::setMinTravel(float travelCm) {
    if (travelCm > 0.0f) {
        minTravelCm_ = travelCm;
    }
}

void RepTracker::setMaxIdleMs(uint64_t maxIdleMs) {
    if (maxIdleMs > 0) {
        maxRepIdleMs_ = maxIdleMs;
    }
}

bool RepTracker::update(float distance, uint64_t nowMs) {
    if (std::isnan(bottomRef_)) {
        bottomRef_ = distance;
        peakDistance_ = distance;
        descentTarget_ = distance;
        lastDistance_ = distance;
        lastMovementMs_ = nowMs;
#if DISTANCE_STREAM_DEBUG
#ifdef ARDUINO
        Serial.println("[REP] INIT | dist=0.0 | bottom=0.0 | peak=0.0 | phase=WaitingBottom | reps=0");
#endif
#endif
        return false;
    }

    float delta = distance - lastDistance_;
    if (std::fabs(delta) > 0.2f) {
        lastMovementMs_ = nowMs;
    }
    lastDistance_ = distance;

    if (distance > bottomRef_) {
        bottomRef_ = distance;
    }

    bool repCompleted = false;
    const char* phaseStr = "WaitingBottom";

    switch (phase_) {
        case Phase::WaitingBottom: {
            float climb = bottomRef_ - distance;
            if (climb >= minTravelCm_) {
                phase_ = Phase::Ascending;
                peakDistance_ = distance;
                descentTarget_ = bottomRef_;
#if DISTANCE_STREAM_DEBUG
#ifdef ARDUINO
                Serial.println("[REP] PHASE: WaitingBottom -> Ascending");
#endif
#endif
            }
            phaseStr = "WaitingBottom";
            break;
        }
        case Phase::Ascending: {
            if (distance < peakDistance_) {
                peakDistance_ = distance;
            }
            float rise = distance - peakDistance_;
            if (rise >= minTravelCm_ * 0.3f) {
                phase_ = Phase::Descending;
#if DISTANCE_STREAM_DEBUG
#ifdef ARDUINO
                Serial.println("[REP] PHASE: Ascending -> Descending");
#endif
#endif
            }
            phaseStr = "Ascending";
            break;
        }
        case Phase::Descending: {
            if (distance > bottomRef_) {
                bottomRef_ = distance;
            }
            if (distance >= descentTarget_ - (minTravelCm_ * 0.25f)) {
                reps_++;
                phase_ = Phase::WaitingBottom;
                bottomRef_ = distance;
                peakDistance_ = distance;
                descentTarget_ = distance;
                lastMovementMs_ = nowMs;
                repCompleted = true;
#if DISTANCE_STREAM_DEBUG
#ifdef ARDUINO
                Serial.println("[REP] *** REP COMPLETED ***");
#endif
#endif
            }
            phaseStr = "Descending";
            break;
        }
    }

    if (nowMs - lastMovementMs_ > maxRepIdleMs_) {
        phase_ = Phase::WaitingBottom;
        bottomRef_ = distance;
        peakDistance_ = distance;
        descentTarget_ = distance;
#if DISTANCE_STREAM_DEBUG
#ifdef ARDUINO
        Serial.println("[REP] TIMEOUT: Reset to WaitingBottom");
#endif
#endif
    }

#if DISTANCE_STREAM_DEBUG
#ifdef ARDUINO
    Serial.print("[REP] dist=");
    Serial.print(distance, 1);
    Serial.print(" | bottom=");
    Serial.print(bottomRef_, 1);
    Serial.print(" | peak=");
    Serial.print(peakDistance_, 1);
    Serial.print(" | phase=");
    Serial.print(phaseStr);
    Serial.print(" | reps=");
    Serial.println(reps_);
#endif
#endif

    return repCompleted;
}

void TestModeSimulator::reset() {
    active_ = false;
    distanceCm_ = bottom_;
    movingUp_ = true;
}

bool TestModeSimulator::generate(uint32_t tagId, AprilTagDetection& detection) {
    active_ = true;
    if (movingUp_) {
        distanceCm_ -= step_;
        if (distanceCm_ <= top_) {
            distanceCm_ = top_;
            movingUp_ = false;
        }
    } else {
        distanceCm_ += step_;
        if (distanceCm_ >= bottom_) {
            distanceCm_ = bottom_;
            movingUp_ = true;
        }
    }

    detection.tagId = tagId;
    detection.distanceCm = distanceCm_;
    return true;
}

CuffController::CuffController(const ControllerConfig& config, SendCallback sendFn)
    : config_(config), send_(std::move(sendFn)) {
    testMode_ = config_.defaultTestMode;
    targetFps_ = config_.defaultFps;
    repTracker_.setMinTravel(config_.defaultMinTravelCm);
    repTracker_.setMaxIdleMs(config_.maxRepIdleMs);

    PersistentSettings stored;
    if (loadPersistentSettings(stored)) {
        if (stored.hasTargetFps) {
            targetFps_ = stored.targetFps;
        }
        if (stored.hasLoiterFps) {
            config_.loiterFps = stored.loiterFps;
        }
        if (stored.hasMinTravelCm) {
            repTracker_.setMinTravel(stored.minTravelCm);
        }
        if (stored.hasMaxRepIdleMs) {
            config_.maxRepIdleMs = stored.maxRepIdleMs;
            repTracker_.setMaxIdleMs(stored.maxRepIdleMs);
        }
    }

    if (testMode_) {
        testSimulator_.reset();
    }
}

void CuffController::setTestMode(bool enabled, uint64_t nowMs) {
    if (testMode_ == enabled) {
        return;
    }
    testMode_ = enabled;
    if (testMode_) {
        notifyStatus("testModeEnabled", nowMs);
    } else {
        testSimulator_.reset();
        session_.reset();
        deviceMode_ = DeviceMode::Idle;
        notifyStatus("testModeDisabled", nowMs);
    }
}

void CuffController::setTargetFps(float fps, uint64_t nowMs) {
    if (fps > 0.1f && fps <= 30.0f) {
        targetFps_ = fps;
        storeTargetFps(fps);
        notifyStatus("fpsUpdated", nowMs);
    }
}

void CuffController::setLoiterFps(float fps, uint64_t nowMs) {
    if (fps > 0.05f && fps <= 10.0f) {
        config_.loiterFps = fps;
        storeLoiterFps(fps);
        notifyStatus("loiterFpsUpdated", nowMs);
    }
}

void CuffController::setMaxRepIdleMs(uint32_t value, uint64_t nowMs) {
    if (value >= 500 && value <= 60000) {
        config_.maxRepIdleMs = value;
        repTracker_.setMaxIdleMs(value);
        storeMaxRepIdleMs(value);
        notifyStatus("repIdleUpdated", nowMs);
    }
}

void CuffController::setMinTravel(float cm, uint64_t nowMs) {
    if (cm >= 1.0f && cm <= 100.0f) {
        repTracker_.setMinTravel(cm);
        storeMinTravelCm(cm);
        notifyStatus("minTravelUpdated", nowMs);
    }
}

void CuffController::resetReps(uint64_t nowMs) {
    repTracker_.reset();
    notifyStatus("repsReset", nowMs);
}

float CuffController::frameIntervalMs() const {
    constexpr float kMinScanFps = 4.0f;
    float fps = (deviceMode_ == DeviceMode::Loiter) ? config_.loiterFps : targetFps_;
    bool clamp = false;
    if (deviceMode_ != DeviceMode::Loiter && fps > 0.0f && fps < kMinScanFps) {
        clamp = true;
        fps = kMinScanFps;
    }
    if (fps <= 0.0f) {
        return 125.0f;
    }
    float interval = 1000.0f / fps;
    // Cap maximum FPS to avoid starving other tasks
    interval = (interval < 10.0f) ? 10.0f : interval;

#ifdef ARDUINO
    static bool s_logged = false;
#if DISTANCE_STREAM_DEBUG
    // In debug mode, log every time to see mode changes
    static uint32_t lastFpsLog = 0;
    uint32_t now = millis();
    if (now - lastFpsLog > 5000) {
        Serial.print("[FPS] mode=");
        switch (deviceMode_) {
            case DeviceMode::Idle: Serial.print("Idle"); break;
            case DeviceMode::AwaitingExercise: Serial.print("AwaitingExercise"); break;
            case DeviceMode::Scanning: Serial.print("Scanning"); break;
            case DeviceMode::Loiter: Serial.print("Loiter"); break;
        }
        Serial.print(" | fps=");
        Serial.print(fps, 2);
        Serial.print(" | interval_ms=");
        Serial.println(interval, 1);
        lastFpsLog = now;
    }
#else
    if (!s_logged) {
        if (clamp) {
            Serial.println("[FPS] Requested <4 fps; clamped to 4 fps for scanning");
        } else {
            Serial.print("[FPS] Scanning fps=");
            Serial.println((deviceMode_ == DeviceMode::Loiter) ? config_.loiterFps : targetFps_);
        }
        s_logged = true;
    }
#endif
#endif

    return interval;
}

void CuffController::handleDetection(const AprilTagDetection& detection, uint64_t nowMs) {
    if (!session_.active || session_.tagId != detection.tagId) {
        session_.reset();
        session_.active = true;
        session_.tagId = detection.tagId;
        session_.lastSeenMs = nowMs;
        session_.metadataReady = false;
        session_.requestSent = false;
        session_.lastRequestMs = 0;
        repTracker_.reset();
        deviceMode_ = DeviceMode::AwaitingExercise;
        sendTagAnnouncement(detection.tagId, nowMs, testMode_);
        notifyStatus("awaitingExercise", nowMs);

        // Debug output
#if !DISTANCE_STREAM_DEBUG
#ifdef ARDUINO
        Serial.println("[CONTROLLER] New AprilTag session");
        Serial.print("[CONTROLLER] Tag ID: ");
        Serial.println(detection.tagId);
#endif
#endif
    }

    session_.lastSeenMs = nowMs;

#if DISTANCE_STREAM_DEBUG
#ifdef ARDUINO
    Serial.print("[");
    Serial.print(nowMs);
    Serial.print("ms] [DETECTION] tag_id=");
    Serial.print(detection.tagId);
    Serial.print(" | distance_cm=");
    Serial.print(detection.distanceCm, 1);
    Serial.print(" | metadata_ready=");
    Serial.print(session_.metadataReady ? "true" : "false");
    Serial.print(" | mode=");
    switch (deviceMode_) {
        case DeviceMode::Idle: Serial.println("Idle"); break;
        case DeviceMode::AwaitingExercise: Serial.println("AwaitingExercise"); break;
        case DeviceMode::Scanning: Serial.println("Scanning"); break;
        case DeviceMode::Loiter: Serial.println("Loiter"); break;
    }
#endif
#endif

    if (!session_.metadataReady) {
        if (testMode_) {
            applyExerciseMetadata(detection.tagId, config_.testExerciseName, config_.testExerciseMetadata, nowMs);
#if !DISTANCE_STREAM_DEBUG
#ifdef ARDUINO
            Serial.println("[CONTROLLER] Test exercise metadata applied");
#endif
#endif
        } else if (!session_.requestSent || (nowMs - session_.lastRequestMs) >= 1000) {
            sendExerciseRequest(detection.tagId, nowMs);
            session_.requestSent = true;
            session_.lastRequestMs = nowMs;
#if !DISTANCE_STREAM_DEBUG
#ifdef ARDUINO
            Serial.println("[CONTROLLER] Requested exercise metadata from mobile app");
#endif
#endif
        }
        // Don't return - continue to rep tracking even without metadata
    }

    exitLoiter(nowMs);

    // Always run rep tracking once session is active, even without metadata
    if (repTracker_.update(detection.distanceCm, nowMs)) {
#ifdef ARDUINO
        Serial.println("[REPS] -------------------------------");
        Serial.print("[REPS] Rep #: ");
        Serial.println(repTracker_.count());
        printSanitizedLine("[REPS] Exercise: ", session_.name);
#endif
        sendRep(nowMs);
    }

    // Only send scan events if metadata is ready
    if (session_.metadataReady) {
        sendScan(detection, nowMs);
    }
}

void CuffController::evaluateTimeouts(uint64_t nowMs) {
    if (session_.active && (nowMs - session_.lastSeenMs) > config_.tagLostMs) {
        enterLoiter(nowMs);
    }
}

void CuffController::maintainTestMode(uint64_t nowMs) {
    if (!testMode_) {
        return;
    }

    if (!testSimulator_.active() || !session_.active) {
        startTestSession(nowMs);
    }
}

void CuffController::startTestSession(uint64_t nowMs) {
    session_.reset();
    session_.active = true;
    session_.tagId = config_.testExerciseId;
    session_.lastSeenMs = nowMs;
    repTracker_.reset();
    deviceMode_ = DeviceMode::AwaitingExercise;

    sendExerciseBroadcast(config_.testExerciseId, config_.testExerciseName, config_.testExerciseMetadata, nowMs, true);
    applyExerciseMetadata(config_.testExerciseId, config_.testExerciseName, config_.testExerciseMetadata, nowMs);
    testSimulator_.reset();
}

void CuffController::applyExerciseMetadata(uint32_t tagId, const std::string& name, const MetadataList& metadata, uint64_t nowMs) {
    session_.tagId = tagId;
    session_.metadataReady = true;
    session_.requestSent = true;
    session_.lastRequestMs = nowMs;
    session_.name = name;
    session_.metadata = metadata;
    if (session_.metadata.size() > kMaxMetadataEntries) {
        session_.metadata.resize(kMaxMetadataEntries);
    }
    session_.lastSeenMs = nowMs;
    deviceMode_ = DeviceMode::Scanning;
    notifyStatus("scanning", nowMs);
}

void CuffController::handleExercisePayload(const ExercisePayload& payload, uint64_t nowMs) {
    if (!session_.active || session_.tagId != payload.id) {
        return;
    }

    if (payload.minTravelCm.has_value()) {
        setMinTravel(payload.minTravelCm.value(), nowMs);
    }
    if (payload.fps.has_value()) {
        setTargetFps(payload.fps.value(), nowMs);
    }

    applyExerciseMetadata(payload.id, payload.name, payload.metadata, nowMs);

    if (!send_) {
        return;
    }

    com_gymjot_cuff_DeviceEvent evt = com_gymjot_cuff_DeviceEvent_init_default;
    evt.timestamp_ms = nowMs;
    evt.which_event = com_gymjot_cuff_DeviceEvent_exercise_ready_tag;
    evt.event.exercise_ready.exercise_id = payload.id;
    send_(evt);
}

void CuffController::notifyStatus(const std::string& status, uint64_t nowMs) {
    if (!send_) {
        return;
    }

    com_gymjot_cuff_DeviceEvent evt = com_gymjot_cuff_DeviceEvent_init_default;
    evt.timestamp_ms = nowMs;
    evt.which_event = com_gymjot_cuff_DeviceEvent_status_tag;

    auto& statusMsg = evt.event.status;
    copyString(status, statusMsg.status_label, sizeof(statusMsg.status_label));
    statusMsg.mode = toProto(deviceMode_);
    statusMsg.fps = (deviceMode_ == DeviceMode::Loiter) ? config_.loiterFps : targetFps_;
    statusMsg.test_mode = testMode_;

    send_(evt);
}

void CuffController::sendTagAnnouncement(uint32_t tagId, uint64_t nowMs, bool fromTest) {
    if (!send_) {
        return;
    }

    com_gymjot_cuff_DeviceEvent evt = com_gymjot_cuff_DeviceEvent_init_default;
    evt.timestamp_ms = nowMs;
    evt.which_event = com_gymjot_cuff_DeviceEvent_tag_tag;

    auto& tagMsg = evt.event.tag;
    tagMsg.tag_id = tagId;
    tagMsg.from_test_mode = fromTest;

    send_(evt);
}

void CuffController::sendExerciseRequest(uint32_t tagId, uint64_t nowMs) {
    if (!send_) {
        return;
    }

    com_gymjot_cuff_DeviceEvent evt = com_gymjot_cuff_DeviceEvent_init_default;
    evt.timestamp_ms = nowMs;
    evt.which_event = com_gymjot_cuff_DeviceEvent_exercise_request_tag;
    evt.event.exercise_request.tag_id = tagId;

    send_(evt);
}

void CuffController::sendExerciseBroadcast(uint32_t id, const std::string& name, const MetadataList& metadata, uint64_t nowMs, bool fromTest) {
    if (!send_) {
        return;
    }

    com_gymjot_cuff_DeviceEvent evt = com_gymjot_cuff_DeviceEvent_init_default;
    evt.timestamp_ms = nowMs;
    evt.which_event = com_gymjot_cuff_DeviceEvent_exercise_broadcast_tag;

    auto& broadcast = evt.event.exercise_broadcast;
    broadcast.exercise_id = id;
    broadcast.from_test_mode = fromTest;
    copyString(name, broadcast.name, sizeof(broadcast.name));
    populateMetadata(metadata, broadcast.metadata);
    broadcast.has_metadata = broadcast.metadata.entries_count > 0;

    send_(evt);
}

void CuffController::sendScan(const AprilTagDetection& detection, uint64_t nowMs) {
    if (!send_) {
        return;
    }

    com_gymjot_cuff_DeviceEvent evt = com_gymjot_cuff_DeviceEvent_init_default;
    evt.timestamp_ms = nowMs;
    evt.which_event = com_gymjot_cuff_DeviceEvent_scan_tag;

    auto& scan = evt.event.scan;
    scan.tag_id = detection.tagId;
    scan.distance_cm = detection.distanceCm;
    scan.mode = toProto(deviceMode_);
    scan.fps = (deviceMode_ == DeviceMode::Loiter) ? config_.loiterFps : targetFps_;

    if (session_.metadataReady && !session_.name.empty()) {
        scan.has_exercise_name = true;
        copyString(session_.name, scan.exercise_name, sizeof(scan.exercise_name));
    } else {
        scan.has_exercise_name = false;
        scan.exercise_name[0] = '\0';
    }

    send_(evt);
}

void CuffController::sendRep(uint64_t nowMs) {
    if (!send_) {
        return;
    }

    com_gymjot_cuff_DeviceEvent evt = com_gymjot_cuff_DeviceEvent_init_default;
    evt.timestamp_ms = nowMs;
    evt.which_event = com_gymjot_cuff_DeviceEvent_rep_tag;

    auto& rep = evt.event.rep;
    rep.tag_id = session_.tagId;
    rep.rep_count = repTracker_.count();

    if (session_.metadataReady && !session_.name.empty()) {
        rep.has_exercise_name = true;
        copyString(session_.name, rep.exercise_name, sizeof(rep.exercise_name));
    } else {
        rep.has_exercise_name = false;
        rep.exercise_name[0] = '\0';
    }

    send_(evt);
}

void CuffController::enterLoiter(uint64_t nowMs) {
    if (deviceMode_ != DeviceMode::Loiter) {
        deviceMode_ = DeviceMode::Loiter;
        notifyStatus("loiter", nowMs);
    }
}

void CuffController::exitLoiter(uint64_t nowMs) {
    if (deviceMode_ == DeviceMode::Loiter) {
        // Exit loiter and go to scanning if metadata ready, otherwise AwaitingExercise
        deviceMode_ = session_.metadataReady ? DeviceMode::Scanning : DeviceMode::AwaitingExercise;
        notifyStatus(session_.metadataReady ? "scanning" : "awaitingExercise", nowMs);
    }
}

}  // namespace gymjot

