#include "config/SafetyConfig.h"
#include "LoggingMacros.h"
#include <Preferences.h>

static const char* TAG = "SafetyConfig";

namespace SafetyConfig {
    // Runtime values initialized to defaults
    uint32_t pumpProtectionMs = Defaults::PUMP_PROTECTION_MS;
    uint32_t sensorStaleMs = Defaults::SENSOR_STALE_MS;
    uint32_t postPurgeMs = Defaults::POST_PURGE_MS;
    uint32_t errorRecoveryMs = Defaults::ERROR_RECOVERY_MS;  // M4: configurable error recovery delay
    int32_t pidIntegralMin = Defaults::PID_INTEGRAL_MIN;     // M1: PID anti-windup lower limit
    int32_t pidIntegralMax = Defaults::PID_INTEGRAL_MAX;     // M1: PID anti-windup upper limit

    static Preferences prefs;
    static const char* NVS_NAMESPACE = "safety";

    void loadFromNVS() {
        // Open in read-write mode to auto-create namespace if it doesn't exist
        // (read-only mode fails with NOT_FOUND on first boot, causing confusing error logs)
        prefs.begin(NVS_NAMESPACE, false);
        pumpProtectionMs = prefs.getUInt("pump_prot", Defaults::PUMP_PROTECTION_MS);
        sensorStaleMs = prefs.getUInt("sensor_stale", Defaults::SENSOR_STALE_MS);
        postPurgeMs = prefs.getUInt("post_purge", Defaults::POST_PURGE_MS);
        errorRecoveryMs = prefs.getUInt("err_recov", Defaults::ERROR_RECOVERY_MS);  // M4
        pidIntegralMin = prefs.getInt("pid_int_min", Defaults::PID_INTEGRAL_MIN);   // M1
        pidIntegralMax = prefs.getInt("pid_int_max", Defaults::PID_INTEGRAL_MAX);   // M1
        prefs.end();

        // Validate loaded values
        if (pumpProtectionMs < Limits::PUMP_PROTECTION_MIN_MS ||
            pumpProtectionMs > Limits::PUMP_PROTECTION_MAX_MS) {
            LOG_WARN(TAG, "Invalid pump_prot %lu ms in NVS, using default %lu ms",
                     pumpProtectionMs, Defaults::PUMP_PROTECTION_MS);
            pumpProtectionMs = Defaults::PUMP_PROTECTION_MS;
        }

        if (sensorStaleMs < Limits::SENSOR_STALE_MIN_MS ||
            sensorStaleMs > Limits::SENSOR_STALE_MAX_MS) {
            LOG_WARN(TAG, "Invalid sensor_stale %lu ms in NVS, using default %lu ms",
                     sensorStaleMs, Defaults::SENSOR_STALE_MS);
            sensorStaleMs = Defaults::SENSOR_STALE_MS;
        }

        if (postPurgeMs < Limits::POST_PURGE_MIN_MS ||
            postPurgeMs > Limits::POST_PURGE_MAX_MS) {
            LOG_WARN(TAG, "Invalid post_purge %lu ms in NVS, using default %lu ms",
                     postPurgeMs, Defaults::POST_PURGE_MS);
            postPurgeMs = Defaults::POST_PURGE_MS;
        }

        // M4: Validate error recovery delay
        if (errorRecoveryMs < Limits::ERROR_RECOVERY_MIN_MS ||
            errorRecoveryMs > Limits::ERROR_RECOVERY_MAX_MS) {
            LOG_WARN(TAG, "Invalid err_recov %lu ms in NVS, using default %lu ms",
                     errorRecoveryMs, Defaults::ERROR_RECOVERY_MS);
            errorRecoveryMs = Defaults::ERROR_RECOVERY_MS;
        }

        // M1: Validate PID integral limits
        if (pidIntegralMin < Limits::PID_INTEGRAL_MIN_LIMIT ||
            pidIntegralMin > 0) {  // Lower limit must be <= 0
            LOG_WARN(TAG, "Invalid pid_int_min %ld in NVS, using default %ld",
                     (long)pidIntegralMin, (long)Defaults::PID_INTEGRAL_MIN);
            pidIntegralMin = Defaults::PID_INTEGRAL_MIN;
        }
        if (pidIntegralMax > Limits::PID_INTEGRAL_MAX_LIMIT ||
            pidIntegralMax < 0) {  // Upper limit must be >= 0
            LOG_WARN(TAG, "Invalid pid_int_max %ld in NVS, using default %ld",
                     (long)pidIntegralMax, (long)Defaults::PID_INTEGRAL_MAX);
            pidIntegralMax = Defaults::PID_INTEGRAL_MAX;
        }

        LOG_INFO(TAG, "Loaded safety config: pump_prot=%lu, sensor_stale=%lu, post_purge=%lu, err_recov=%lu ms",
                 pumpProtectionMs, sensorStaleMs, postPurgeMs, errorRecoveryMs);
        LOG_INFO(TAG, "PID integral limits: min=%ld, max=%ld (scaled x1000)",
                 (long)pidIntegralMin, (long)pidIntegralMax);
    }

    void saveToNVS() {
        prefs.begin(NVS_NAMESPACE, false);  // read-write
        prefs.putUInt("pump_prot", pumpProtectionMs);
        prefs.putUInt("sensor_stale", sensorStaleMs);
        prefs.putUInt("post_purge", postPurgeMs);
        prefs.putUInt("err_recov", errorRecoveryMs);  // M4
        prefs.putInt("pid_int_min", pidIntegralMin);  // M1
        prefs.putInt("pid_int_max", pidIntegralMax);  // M1
        prefs.end();

        LOG_INFO(TAG, "Saved safety config to NVS: pump_prot=%lu, sensor_stale=%lu, post_purge=%lu, err_recov=%lu ms",
                 pumpProtectionMs, sensorStaleMs, postPurgeMs, errorRecoveryMs);
        LOG_INFO(TAG, "PID integral limits saved: min=%ld, max=%ld",
                 (long)pidIntegralMin, (long)pidIntegralMax);
    }

    bool setPumpProtection(uint32_t ms) {
        if (ms < Limits::PUMP_PROTECTION_MIN_MS || ms > Limits::PUMP_PROTECTION_MAX_MS) {
            LOG_WARN(TAG, "Invalid pump_prot %lu ms (range: %lu-%lu ms)",
                     ms, Limits::PUMP_PROTECTION_MIN_MS, Limits::PUMP_PROTECTION_MAX_MS);
            return false;
        }
        pumpProtectionMs = ms;
        LOG_INFO(TAG, "Set pump_prot to %lu ms", ms);
        return true;
    }

    bool setSensorStale(uint32_t ms) {
        if (ms < Limits::SENSOR_STALE_MIN_MS || ms > Limits::SENSOR_STALE_MAX_MS) {
            LOG_WARN(TAG, "Invalid sensor_stale %lu ms (range: %lu-%lu ms)",
                     ms, Limits::SENSOR_STALE_MIN_MS, Limits::SENSOR_STALE_MAX_MS);
            return false;
        }
        sensorStaleMs = ms;
        LOG_INFO(TAG, "Set sensor_stale to %lu ms", ms);
        return true;
    }

    bool setPostPurge(uint32_t ms) {
        if (ms < Limits::POST_PURGE_MIN_MS || ms > Limits::POST_PURGE_MAX_MS) {
            LOG_WARN(TAG, "Invalid post_purge %lu ms (range: %lu-%lu ms)",
                     ms, Limits::POST_PURGE_MIN_MS, Limits::POST_PURGE_MAX_MS);
            return false;
        }
        postPurgeMs = ms;
        LOG_INFO(TAG, "Set post_purge to %lu ms", ms);
        return true;
    }

    // M4: Configurable error recovery delay
    bool setErrorRecovery(uint32_t ms) {
        if (ms < Limits::ERROR_RECOVERY_MIN_MS || ms > Limits::ERROR_RECOVERY_MAX_MS) {
            LOG_WARN(TAG, "Invalid err_recov %lu ms (range: %lu-%lu ms)",
                     ms, Limits::ERROR_RECOVERY_MIN_MS, Limits::ERROR_RECOVERY_MAX_MS);
            return false;
        }
        errorRecoveryMs = ms;
        LOG_INFO(TAG, "Set err_recov to %lu ms", ms);
        return true;
    }

    // M1: Configurable PID anti-windup limits
    bool setPIDIntegralLimits(int32_t min, int32_t max) {
        // Validate range
        if (min < Limits::PID_INTEGRAL_MIN_LIMIT || min > 0) {
            LOG_WARN(TAG, "Invalid pid_int_min %ld (range: %ld to 0)",
                     (long)min, (long)Limits::PID_INTEGRAL_MIN_LIMIT);
            return false;
        }
        if (max > Limits::PID_INTEGRAL_MAX_LIMIT || max < 0) {
            LOG_WARN(TAG, "Invalid pid_int_max %ld (range: 0 to %ld)",
                     (long)max, (long)Limits::PID_INTEGRAL_MAX_LIMIT);
            return false;
        }
        // Ensure min < max
        if (min >= max) {
            LOG_WARN(TAG, "pid_int_min (%ld) must be < pid_int_max (%ld)",
                     (long)min, (long)max);
            return false;
        }

        pidIntegralMin = min;
        pidIntegralMax = max;
        LOG_INFO(TAG, "Set PID integral limits: min=%ld, max=%ld (%.1f to %.1f °C)",
                 (long)min, (long)max, min / 1000.0f, max / 1000.0f);
        return true;
    }
}
