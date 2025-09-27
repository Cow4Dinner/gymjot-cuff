#include "CuffController.h"

#include <cmath>
#include <limits>

namespace gymjot {

void StationSession::reset() {
    active = false;
    metadataReady = false;
    requestSent = false;
    tagId = 0;
    name.clear();
    metadataJson.clear();
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

    switch (phase_) {
        case Phase::WaitingBottom: {
            float climb = bottomRef_ - distance;
            if (climb >= minTravelCm_) {
                phase_ = Phase::Ascending;
                peakDistance_ = distance;
                descentTarget_ = bottomRef_;
            }
            break;
        }
        case Phase::Ascending: {
            if (distance < peakDistance_) {
                peakDistance_ = distance;
            }
            float rise = distance - peakDistance_;
            if (rise >= minTravelCm_ * 0.3f) {
                phase_ = Phase::Descending;
            }
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
                return true;
            }
            break;
        }
    }

    if (nowMs - lastMovementMs_ > maxRepIdleMs_) {
        phase_ = Phase::WaitingBottom;
        bottomRef_ = distance;
        peakDistance_ = distance;
        descentTarget_ = distance;
    }

    return false;
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
        notifyStatus("fpsUpdated", nowMs);
    }
}

void CuffController::resetReps(uint64_t nowMs) {
    repTracker_.reset();
    notifyStatus("repsReset", nowMs);
}

float CuffController::frameIntervalMs() const {
    float fps = (deviceMode_ == DeviceMode::Loiter) ? config_.loiterFps : targetFps_;
    if (fps <= 0.0f) {
        return 125.0f;
    }
    float interval = 1000.0f / fps;
    return (interval < 10.0f) ? 10.0f : interval;
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
        deviceMode_ = DeviceMode::AwaitingStation;
        sendTagAnnouncement(detection.tagId, nowMs, testMode_);
        notifyStatus("awaitingStation", nowMs);
    }

    session_.lastSeenMs = nowMs;

    if (!session_.metadataReady) {
        if (testMode_) {
            applyStationMetadata(detection.tagId, config_.testStationName, config_.testStationMetadata, nowMs);
        } else if (!session_.requestSent || (nowMs - session_.lastRequestMs) >= 1000) {
            sendStationRequest(detection.tagId, nowMs);
            session_.requestSent = true;
            session_.lastRequestMs = nowMs;
        }
        return;
    }

    exitLoiter(nowMs);
    sendScan(detection, nowMs);

    if (repTracker_.update(detection.distanceCm, nowMs)) {
        sendRep(nowMs);
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
    session_.tagId = config_.testStationId;
    session_.lastSeenMs = nowMs;
    repTracker_.reset();
    deviceMode_ = DeviceMode::AwaitingStation;

    sendStationBroadcast(config_.testStationId, config_.testStationName, config_.testStationMetadata, nowMs, "test");
    applyStationMetadata(config_.testStationId, config_.testStationName, config_.testStationMetadata, nowMs);
    testSimulator_.reset();
    AprilTagDetection dummy;
}

void CuffController::applyStationMetadata(uint32_t tagId, const std::string& name, const std::string& metadataJson, uint64_t nowMs) {
    session_.tagId = tagId;
    session_.metadataReady = true;
    session_.requestSent = true;
    session_.lastRequestMs = nowMs;
    session_.name = name;
    session_.metadataJson = metadataJson;
    session_.lastSeenMs = nowMs;
    deviceMode_ = DeviceMode::Scanning;
    notifyStatus("scanning", nowMs);
}

void CuffController::handleStationPayload(const StationPayload& payload, uint64_t nowMs) {
    if (!session_.active || session_.tagId != payload.id) {
        return;
    }

    if (payload.minTravelCm.has_value()) {
        repTracker_.setMinTravel(payload.minTravelCm.value());
    }
    if (payload.fps.has_value()) {
        setTargetFps(payload.fps.value(), nowMs);
    }

    applyStationMetadata(payload.id, payload.name, payload.metadataJson, nowMs);

    StaticJsonDocument<128> ack;
    ack["type"] = "status";
    ack["status"] = "stationReady";
    ack["stationId"] = payload.id;
    ack["timestamp"] = nowMs;
    sendJsonDoc(ack);
}

void CuffController::notifyStatus(const std::string& status, uint64_t nowMs) {
    StaticJsonDocument<192> doc;
    doc["type"] = "status";
    doc["status"] = status.c_str();
    doc["timestamp"] = nowMs;
    doc["mode"] = static_cast<int>(deviceMode_);
    doc["fps"] = (deviceMode_ == DeviceMode::Loiter) ? config_.loiterFps : targetFps_;
    if (testMode_) {
        doc["testMode"] = true;
    }
    sendJsonDoc(doc);
}

void CuffController::sendJsonDoc(const JsonDocument& doc) {
    if (!send_) {
        return;
    }
    std::string payload;
    serializeJson(doc, payload);
    send_(payload);
}

void CuffController::sendTagAnnouncement(uint32_t tagId, uint64_t nowMs, bool fromTest) {
    StaticJsonDocument<192> doc;
    doc["type"] = "tag";
    doc["tagId"] = tagId;
    doc["timestamp"] = nowMs;
    if (fromTest) {
        doc["origin"] = "test";
    }
    sendJsonDoc(doc);
}

void CuffController::sendStationRequest(uint32_t tagId, uint64_t nowMs) {
    StaticJsonDocument<192> doc;
    doc["type"] = "stationRequest";
    doc["tagId"] = tagId;
    doc["timestamp"] = nowMs;
    sendJsonDoc(doc);
}

void CuffController::sendStationBroadcast(uint32_t id, const std::string& name, const std::string& metadataJson, uint64_t nowMs, const char* origin) {
    StaticJsonDocument<256> doc;
    doc["type"] = "station";
    doc["id"] = id;
    doc["name"] = name.c_str();
    if (origin) {
        doc["origin"] = origin;
    }

    if (!metadataJson.empty()) {
        StaticJsonDocument<256> meta;
        if (deserializeJson(meta, metadataJson) == DeserializationError::Ok) {
            doc["metadata"] = meta.as<JsonObject>();
        } else {
            doc["metadata"] = metadataJson.c_str();
        }
    }

    sendJsonDoc(doc);
}

void CuffController::sendScan(const AprilTagDetection& detection, uint64_t nowMs) {
    StaticJsonDocument<256> doc;
    doc["type"] = "scan";
    doc["tagId"] = detection.tagId;
    doc["distanceCm"] = detection.distanceCm;
    doc["timestamp"] = nowMs;
    doc["mode"] = (deviceMode_ == DeviceMode::Loiter) ? "loiter" : "scanning";
    doc["fps"] = (deviceMode_ == DeviceMode::Loiter) ? config_.loiterFps : targetFps_;
    if (session_.metadataReady && !session_.name.empty()) {
        doc["stationName"] = session_.name.c_str();
    }
    sendJsonDoc(doc);
}

void CuffController::sendRep(uint64_t nowMs) {
    StaticJsonDocument<192> doc;
    doc["type"] = "rep";
    doc["count"] = repTracker_.count();
    doc["tagId"] = session_.tagId;
    doc["timestamp"] = nowMs;
    if (session_.metadataReady && !session_.name.empty()) {
        doc["stationName"] = session_.name.c_str();
    }
    sendJsonDoc(doc);
}

void CuffController::enterLoiter(uint64_t nowMs) {
    if (deviceMode_ != DeviceMode::Loiter) {
        deviceMode_ = DeviceMode::Loiter;
        notifyStatus("loiter", nowMs);
    }
}

void CuffController::exitLoiter(uint64_t nowMs) {
    if (deviceMode_ == DeviceMode::Loiter && session_.metadataReady) {
        deviceMode_ = DeviceMode::Scanning;
        notifyStatus("scanning", nowMs);
    }
}

}  // namespace gymjot

