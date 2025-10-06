#pragma once

#include <cstdint>
#include <cstddef>
#include <limits>

namespace gymjot::system {

/**
 * @brief Monitors heap availability and applies adaptive throttling when memory is low.
 *
 * Usage example:
 * @code
 * HeapMonitor monitor({
 *     .serialLogging = false,
 *     .lowHeapThresholdBytes = 60 * 1024,
 *     .recoveryDelayMs = 400,
 *     .logIntervalMs = 5000,
 *     .dropLogThresholdBytes = 8 * 1024
 * });
 *
 * monitor.update("boot", millis(), true);
 * if (!monitor.shouldThrottle(millis())) {
 *     captureFrame();
 * }
 * @endcode
 */
class HeapMonitor {
public:
    struct Config {
        bool serialLogging;
        uint32_t lowHeapThresholdBytes;
        uint32_t recoveryDelayMs;
        uint32_t logIntervalMs;
        uint32_t dropLogThresholdBytes;
    };

    explicit HeapMonitor(const Config& config);

    /**
     * @brief Record the current heap state and optionally emit diagnostics.
     * @param context     Call-site identifier (e.g. "apriltag-loop").
     * @param nowMs       Monotonic timestamp from `millis()`.
     * @param forceLog    When true, diagnostic output is emitted regardless of thresholds.
     */
    void update(const char* context, uint64_t nowMs, bool forceLog = false);

    /**
     * @brief Enable or disable verbose serial logging at runtime.
     */
    void enableSerialLogging(bool enabled);

    /**
     * @brief Indicates whether heavy operations should be deferred because of low heap.
     */
    [[nodiscard]] bool shouldThrottle(uint64_t nowMs) const;

private:
    Config config_;
    bool serialLogging_;
    uint64_t throttleUntilMs_ = 0;
    uint64_t lastThrottleLogMs_ = 0;
#ifdef ENABLE_HEAP_SERIAL_LOGGING
    uint32_t minFreeHeap_ = std::numeric_limits<uint32_t>::max();
    uint32_t minFreePsram_ = std::numeric_limits<uint32_t>::max();
    size_t minLargestBlock_ = std::numeric_limits<size_t>::max();
    uint32_t lastLoggedHeap_ = 0;
    uint32_t lastLoggedPsram_ = 0;
    size_t lastLoggedLargestBlock_ = 0;
    uint64_t lastLogMs_ = 0;
#endif
};

/**
 * @brief Handles deferred restart requests to guarantee graceful shutdowns.
 *
 * Example:
 * @code
 * ResetScheduler scheduler(3000, [](const char* reason) {
 *     esp_task_wdt_reset();
 *     delay(100);
 *     ESP.restart();
 * });
 *
 * scheduler.request("apriltag-rotation", millis());
 * scheduler.service(millis(), cameraBusy);
 * @endcode
 */
class ResetScheduler {
public:
    using Callback = void (*)(const char* reason);

    ResetScheduler(uint32_t gracePeriodMs, Callback callback);

    void request(const char* reason, uint64_t nowMs);
    void service(uint64_t nowMs, bool operationInProgress);
    void cancel();
    [[nodiscard]] bool pending() const { return pending_; }

private:
    uint32_t gracePeriodMs_;
    Callback callback_;
    bool pending_ = false;
    uint64_t scheduledAtMs_ = 0;
    char reason_[32] = {0};
};

}  // namespace gymjot::system
