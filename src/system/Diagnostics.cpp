#include "system/Diagnostics.h"

#ifdef ARDUINO

#include <Arduino.h>
#include <cstring>

#ifdef ENABLE_HEAP_SERIAL_LOGGING
#include <esp_heap_caps.h>
#endif

namespace gymjot::system {

HeapMonitor::HeapMonitor(const Config& config)
    : config_(config), serialLogging_(config.serialLogging) {}

void HeapMonitor::enableSerialLogging(bool enabled) {
#ifdef ENABLE_HEAP_SERIAL_LOGGING
    serialLogging_ = enabled;
#else
    (void)enabled;
    serialLogging_ = false;
#endif
}

void HeapMonitor::update(const char* context, uint64_t nowMs, bool forceLog) {
    const char* safeContext = (context && context[0] != '\0') ? context : "heap";
    const uint32_t freeHeap = ESP.getFreeHeap();

#ifdef ENABLE_HEAP_SERIAL_LOGGING
    const uint32_t freePsram = ESP.getFreePsram();
    const size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);

    if (freeHeap < minFreeHeap_) {
        minFreeHeap_ = freeHeap;
    }
    if (freePsram < minFreePsram_) {
        minFreePsram_ = freePsram;
    }
    if (largestBlock < minLargestBlock_) {
        minLargestBlock_ = largestBlock;
    }

    const bool heapDrop = serialLogging_ && lastLoggedHeap_ > freeHeap &&
        (lastLoggedHeap_ - freeHeap) >= config_.dropLogThresholdBytes;
    const bool psramDrop = serialLogging_ && lastLoggedPsram_ > freePsram &&
        (lastLoggedPsram_ - freePsram) >= config_.dropLogThresholdBytes;
    const bool blockDrop = serialLogging_ && lastLoggedLargestBlock_ > largestBlock &&
        (lastLoggedLargestBlock_ - largestBlock) >= config_.dropLogThresholdBytes;
    const bool periodic = serialLogging_ && (nowMs - lastLogMs_) >= config_.logIntervalMs;

    if (forceLog || heapDrop || psramDrop || blockDrop || periodic) {
        Serial.printf("[HEAP] %s free_heap=%lu min_heap=%lu free_psram=%lu min_psram=%lu largest_block=%lu min_largest_block=%lu\n",
                      safeContext,
                      static_cast<unsigned long>(freeHeap),
                      static_cast<unsigned long>(minFreeHeap_),
                      static_cast<unsigned long>(freePsram),
                      static_cast<unsigned long>(minFreePsram_),
                      static_cast<unsigned long>(largestBlock),
                      static_cast<unsigned long>(minLargestBlock_));

        lastLogMs_ = nowMs;
        lastLoggedHeap_ = freeHeap;
        lastLoggedPsram_ = freePsram;
        lastLoggedLargestBlock_ = largestBlock;
    }
#else
    (void)forceLog;
    (void)safeContext;
#endif

    if (freeHeap <= config_.lowHeapThresholdBytes) {
        const uint64_t candidate = nowMs + config_.recoveryDelayMs;
        if (candidate > throttleUntilMs_) {
            throttleUntilMs_ = candidate;
        }
        if (nowMs - lastThrottleLogMs_ > 2000) {
            Serial.printf("[HEAP] Low free heap detected (%lu bytes) context=%s, deferring AprilTag capture\n",
                          static_cast<unsigned long>(freeHeap), safeContext);
            lastThrottleLogMs_ = nowMs;
        }
    }
}

bool HeapMonitor::shouldThrottle(uint64_t nowMs) const {
    return nowMs < throttleUntilMs_;
}

ResetScheduler::ResetScheduler(uint32_t gracePeriodMs, Callback callback)
    : gracePeriodMs_(gracePeriodMs), callback_(callback) {}

void ResetScheduler::request(const char* reason, uint64_t nowMs) {
    if (!callback_) {
        return;
    }

    const char* safeReason = (reason && reason[0] != '\0') ? reason : "unspecified";
    std::strncpy(reason_, safeReason, sizeof(reason_) - 1);
    reason_[sizeof(reason_) - 1] = '\0';

    pending_ = true;
    scheduledAtMs_ = nowMs + gracePeriodMs_;

    Serial.print("[RESET] Scheduled system reset (reason=");
    Serial.print(reason_);
    Serial.print(") in ");
    Serial.print(gracePeriodMs_);
    Serial.println("ms");
}

void ResetScheduler::service(uint64_t nowMs, bool operationInProgress) {
    if (!pending_) {
        return;
    }

    if (operationInProgress) {
        scheduledAtMs_ = nowMs + gracePeriodMs_;
        return;
    }

    if (nowMs < scheduledAtMs_) {
        return;
    }

    Serial.print("[RESET] Performing scheduled system reset (reason=");
    Serial.print(reason_[0] ? reason_ : "unspecified");
    Serial.println(")");

    callback_(reason_[0] ? reason_ : "unspecified");
    cancel();
}

void ResetScheduler::cancel() {
    pending_ = false;
    scheduledAtMs_ = 0;
    reason_[0] = '\0';
}

}  // namespace gymjot::system

#else  // !ARDUINO

#include <cstdio>
#include <cstring>

namespace gymjot::system {

HeapMonitor::HeapMonitor(const Config& config)
    : config_(config), serialLogging_(false) {}

void HeapMonitor::enableSerialLogging(bool enabled) {
    serialLogging_ = enabled;
}

void HeapMonitor::update(const char* context, uint64_t nowMs, bool forceLog) {
    (void)context;
    (void)nowMs;
    (void)forceLog;
    (void)serialLogging_;
}

bool HeapMonitor::shouldThrottle(uint64_t) const {
    return false;
}

ResetScheduler::ResetScheduler(uint32_t gracePeriodMs, Callback callback)
    : gracePeriodMs_(gracePeriodMs), callback_(callback) {}

void ResetScheduler::request(const char* reason, uint64_t nowMs) {
    (void)reason;
    (void)nowMs;
}

void ResetScheduler::service(uint64_t nowMs, bool operationInProgress) {
    (void)nowMs;
    (void)operationInProgress;
    if (pending_ && callback_) {
        callback_(reason_[0] ? reason_ : "unspecified");
    }
    cancel();
}

void ResetScheduler::cancel() {
    pending_ = false;
    scheduledAtMs_ = 0;
    reason_[0] = '\0';
}

}  // namespace gymjot::system

#endif  // ARDUINO
