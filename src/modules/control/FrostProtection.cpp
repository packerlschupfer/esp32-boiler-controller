// src/modules/control/FrostProtection.cpp
#include "modules/control/FrostProtection.h"

#include <atomic>

#include "config/SafetyConfig.h"
#include "config/SystemConstants.h"
#include "core/SystemResourceProvider.h"
#include "modules/control/FrostProtectionPolicy.h"
#include "shared/SharedSensorReadings.h"
#include "shared/Temperature.h"
#include "utils/MutexRetryHelper.h"
#include "LoggingMacros.h"
#include <Arduino.h>

static const char* TAG = "Frost";

static_assert(FrostProtectionPolicy::TRIGGER_TENTHS ==
                  SystemConstants::Temperature::FREEZE_PROTECTION_TEMP_C,
              "frost trigger must stay tied to FREEZE_PROTECTION_TEMP_C");

namespace {
    // Read from both mode tasks, written only by evaluate(). Atomics rather than a mutex: one
    // bool each, and a reader that sees the previous value for one cycle loses nothing - the
    // latch is re-evaluated every cycle anyway.
    std::atomic<bool> s_heat{false};
    std::atomic<bool> s_water{false};

    /**
     * @brief Is a reading recent enough to act on?
     *
     * THIS IS THE WHOLE POINT OF THE 2026-10-25 CHANGE. Nothing clears
     * isBoilerTempOutputValid / isWaterHeaterTempTankValid / isInsideTempValid when reads
     * simply STOP - a dead MB8ART leaves the last value flagged valid indefinitely. So this
     * guard used to read those flags raw, and a bus failure froze the flow reading at whatever
     * it last was, typically 30-40 °C just after a burn. Frost protection could then never
     * fire again however cold the house got, and nothing reported it: `sf` stayed 1 because the
     * fallback is itself only re-evaluated by MB8ART reads.
     *
     * Same threshold as TemperatureSensorFallback (SafetyConfig::sensorStaleMs) so there is one
     * number for "too old to act on" rather than two that can drift apart.
     */
    inline bool fresh(uint32_t stampMs, uint32_t nowMs) {
        if (stampMs == 0) {
            return false;  // never written: no reading has ever arrived
        }
        return (nowMs - stampMs) <= SafetyConfig::sensorStaleMs;
    }
}  // namespace

namespace FrostProtection {

void evaluate() {
    FrostProtectionPolicy::Readings r;
    {
        auto guard = MutexRetryHelper::acquireGuard(SRP::getSensorReadingsMutex(),
                                                    "SensorReadings-Frost");
        if (!guard) {
            return;  // no snapshot: leave both latches as they are rather than guessing
        }
        const SharedSensorReadings& sr = SRP::getSensorReadings();
        const uint32_t now = millis();

        // Per-source freshness. Flow and tank are MB8ART channels, so they share
        // lastBoilerTempUpdateTimestamp. The room is an ANDRTF3 channel and uses
        // insideTempGoodMs - NOT the shared lastUpdateTimestamp, which the MB8ART also writes
        // and which would therefore let a live MB8ART mask a dead room sensor. That is the
        // same mistake F5 fixed on the boiler side; insideTempGoodMs exists precisely because
        // isInsideTempValid stays true through ~15 s of failed reads.
        const bool mb8artFresh = fresh(sr.lastBoilerTempUpdateTimestamp, now);
        const bool roomFresh = fresh(sr.insideTempGoodMs, now);

        r.flow = sr.boilerTempOutput;
        r.flowUsable = sr.isBoilerTempOutputValid && mb8artFresh;
        r.tank = sr.waterHeaterTempTank;
        r.tankUsable = sr.isWaterHeaterTempTankValid && mb8artFresh;
        r.room = sr.insideTemp;
        r.roomUsable = sr.isInsideTempValid && roomFresh;
    }

    const bool wasHeat = s_heat.load(std::memory_order_relaxed);
    const bool wasWater = s_water.load(std::memory_order_relaxed);
    const bool nowHeat = FrostProtectionPolicy::heatDemand(wasHeat, r);
    const bool nowWater = FrostProtectionPolicy::waterDemand(wasWater, r);

    if (nowHeat == wasHeat && nowWater == wasWater) {
        return;  // edge-triggered logging: one line per transition, not per cycle
    }
    s_heat.store(nowHeat, std::memory_order_relaxed);
    s_water.store(nowWater, std::memory_order_relaxed);

    char flowBuf[16], tankBuf[16], roomBuf[16];
    formatTemp(flowBuf, sizeof(flowBuf), r.flow);
    formatTemp(tankBuf, sizeof(tankBuf), r.tank);
    formatTemp(roomBuf, sizeof(roomBuf), r.room);
    const uint8_t cold = FrostProtectionPolicy::coldMask(r);

    if (nowHeat != wasHeat) {
        if (nowHeat) {
            LOG_WARN(TAG, "FROST HEAT ON (%s cold) - flow %s°C usable=%d, room %s°C usable=%d; "
                          "requesting minimum flow regardless of the heating switch",
                     FrostProtectionPolicy::describe(cold), flowBuf, r.flowUsable ? 1 : 0,
                     roomBuf, r.roomUsable ? 1 : 0);
        } else {
            LOG_WARN(TAG, "Frost heat off - flow %s°C, room %s°C", flowBuf, roomBuf);
        }
    }
    if (nowWater != wasWater) {
        if (nowWater) {
            LOG_WARN(TAG, "FROST WATER ON - tank %s°C usable=%d; requesting a charge regardless "
                          "of the water switch",
                     tankBuf, r.tankUsable ? 1 : 0);
        } else {
            LOG_WARN(TAG, "Frost water off - tank %s°C", tankBuf);
        }
    }
}

bool heatDemand() {
    return s_heat.load(std::memory_order_relaxed);
}

bool waterDemand() {
    return s_water.load(std::memory_order_relaxed);
}

}  // namespace FrostProtection
