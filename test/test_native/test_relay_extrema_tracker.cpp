/**
 * @file test_relay_extrema_tracker.cpp
 * @brief Unit tests for RelayExtrema::Tracker (autotune peak/trough detection)
 */

#include <unity.h>
#include <vector>

#include "../../include/modules/control/RelayExtremaTracker.h"

// setUp and tearDown are defined in test_main.cpp

namespace {

// Replays a relay test against a lagging plant: after each relay switch the
// temperature keeps moving in the old direction for `lagSamples` samples.
// Mirrors the 2026-09-14 water run: relay OFF at 61C but boiler peaks ~65.5C,
// relay ON at 59C but boiler dips to ~57C.
struct Recorded {
    std::vector<float> peaks, peakTimes, troughs, troughTimes;
};

// relayStartsOn=false reproduces PIDAutoTuner: relay starts OFF and switches
// ON at the very first sample when the boiler is cold.
// sampleBeforeSwitch=true reproduces PIDAutoTuner::relayControl(): sample() is
// called for every sample and onSwitch() additionally on switch samples.
Recorded runLaggingPlant(float startTemp, int halfCycles, bool relayStartsOn,
                         bool sampleBeforeSwitch) {
    const float setpoint = 60.0f, hysteresis = 1.0f;
    const float riseRate = 0.9f, fallRate = 0.6f;   // C per sample
    const int lagSamples = 5;

    RelayExtrema::Tracker tracker;
    Recorded rec;
    float temp = startTemp;
    bool relayOn = relayStartsOn;
    bool heating = true;       // what the plant actually does (lags the relay)
    int lagLeft = 0;
    float t = 0.0f;
    int switches = 0;

    while (switches < halfCycles && t < 10000.0f) {
        if (t > 0.0f) {
            temp += heating ? riseRate : -fallRate;
        }
        t += 1.0f;

        if (sampleBeforeSwitch) {
            tracker.sample(temp, t);
        }

        bool newRelay = relayOn;
        if (relayOn && temp > setpoint + hysteresis) newRelay = false;
        if (!relayOn && temp < setpoint - hysteresis) newRelay = true;

        if (newRelay != relayOn) {
            RelayExtrema::Event e = tracker.onSwitch(newRelay, temp, t);
            if (e.valid) {
                if (e.isPeak) { rec.peaks.push_back(e.value); rec.peakTimes.push_back(e.time); }
                else          { rec.troughs.push_back(e.value); rec.troughTimes.push_back(e.time); }
            }
            relayOn = newRelay;
            lagLeft = lagSamples;
            switches++;
        } else if (!sampleBeforeSwitch) {
            tracker.sample(temp, t);
        }

        if (lagLeft > 0) {
            lagLeft--;
            if (lagLeft == 0) heating = relayOn;
        }
    }
    return rec;
}

} // namespace

void test_relay_extrema_peaks_include_post_switch_overshoot() {
    Recorded rec = runLaggingPlant(25.0f, 9, false, true);

    TEST_ASSERT_TRUE(rec.peaks.size() >= 3);
    for (float p : rec.peaks) {
        // Relay switches OFF just above 61C; the lagging plant climbs ~4.5C more.
        TEST_ASSERT_TRUE(p > 64.0f);
    }
}

void test_relay_extrema_troughs_include_post_switch_undershoot() {
    Recorded rec = runLaggingPlant(25.0f, 9, false, true);

    TEST_ASSERT_TRUE(rec.troughs.size() >= 3);
    for (float v : rec.troughs) {
        // Relay switches ON just below 59C; the plant keeps falling ~3C more.
        TEST_ASSERT_TRUE(v < 57.0f);
    }
}

void test_relay_extrema_ignores_cold_start_phase() {
    // All combinations of relay state at start and call order. The case
    // relayStartsOn=false + sampleBeforeSwitch=true is the real PIDAutoTuner
    // (2026-09-14 heating run recorded the 53.0C start temperature as a trough).
    for (bool relayStartsOn : {true, false}) {
        for (bool sampleBeforeSwitch : {true, false}) {
            Recorded rec = runLaggingPlant(25.0f, 10, relayStartsOn, sampleBeforeSwitch);
            TEST_ASSERT_TRUE(rec.troughs.size() >= 3);
            for (float v : rec.troughs) {
                TEST_ASSERT_TRUE(v > 50.0f);
            }
        }
    }
}

void test_relay_extrema_extreme_times_are_after_switch() {
    Recorded rec = runLaggingPlant(25.0f, 9, false, true);

    // Peaks and troughs must alternate in time (peak of OFF phase, then trough
    // of the following ON phase), giving a usable period.
    TEST_ASSERT_TRUE(rec.peakTimes.size() >= 2);
    TEST_ASSERT_TRUE(rec.troughTimes.size() >= 1);
    TEST_ASSERT_TRUE(rec.peakTimes[1] > rec.peakTimes[0]);
    TEST_ASSERT_TRUE(rec.troughTimes[0] > rec.peakTimes[0]);
}
