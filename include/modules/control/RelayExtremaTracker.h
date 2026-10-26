// include/modules/control/RelayExtremaTracker.h
#ifndef RELAY_EXTREMA_TRACKER_H
#define RELAY_EXTREMA_TRACKER_H

/**
 * @brief Peak/trough detection for relay-feedback autotuning of a lagging plant.
 *
 * The boiler keeps heating after the relay switches OFF (burner minimum on-time,
 * heat stored in the exchanger) and keeps cooling after it switches ON
 * (pre-purge, ignition). The true peak therefore lies inside the OFF phase and
 * the true trough inside the ON phase. Recording the extreme of the phase that
 * *precedes* each switch (the old behaviour) captured only the values at the
 * switch point: on 2026-09-14 the tuner saw 61.3/58.8C while the boiler really
 * swung 65.6/56.9C, so amplitude and Ku were ~3.4x off.
 *
 * Usage per sample: sample(temp, time); on a relay switch additionally
 * onSwitch(newRelayOn, temp, time), which returns the extreme of the phase that
 * just ended:
 *   - OFF phase ended (relay now ON)  -> peak   = max over that OFF phase
 *   - ON phase ended  (relay now OFF) -> trough = min over that ON phase
 * The initial phase is ignored: both a phase that was not started by a switch
 * and a phase started by a switch at the very first sample (PIDAutoTuner starts
 * with the relay OFF, so a cold boiler switches ON immediately) are warm-up,
 * otherwise the start temperature would be reported as a trough.
 *
 * Calling sample() and then onSwitch() for the same sample is allowed (that is
 * what PIDAutoTuner::relayControl() does): samples are counted by timestamp.
 *
 * Header-only and FreeRTOS-free so it can be unit tested natively.
 */
namespace RelayExtrema {

    struct Event {
        bool valid;
        bool isPeak;   // true = peak, false = trough
        float value;
        float time;
    };

    class Tracker {
    public:
        Tracker() { reset(); }

        void reset() {
            phaseStartedBySwitch_ = false;
            sampleCount_ = 0;
            lastSampleTime_ = 0.0f;
            phaseMax_ = phaseMin_ = 0.0f;
            phaseMaxTime_ = phaseMinTime_ = 0.0f;
        }

        void sample(float temp, float time) {
            if (sampleCount_ == 0) {
                startPhase(temp, time);
                sampleCount_ = 1;
                lastSampleTime_ = time;
                return;
            }
            if (time != lastSampleTime_) {
                sampleCount_++;
                lastSampleTime_ = time;
            }
            if (temp > phaseMax_) {
                phaseMax_ = temp;
                phaseMaxTime_ = time;
            }
            if (temp < phaseMin_) {
                phaseMin_ = temp;
                phaseMinTime_ = time;
            }
        }

        Event onSwitch(bool newRelayOn, float temp, float time) {
            sample(temp, time);
            bool switchAtFirstSample = (sampleCount_ <= 1);

            Event event = {false, false, 0.0f, 0.0f};
            if (phaseStartedBySwitch_) {
                if (newRelayOn) {
                    event = {true, true, phaseMax_, phaseMaxTime_};
                } else {
                    event = {true, false, phaseMin_, phaseMinTime_};
                }
            }

            startPhase(temp, time);
            phaseStartedBySwitch_ = !switchAtFirstSample;
            return event;
        }

    private:
        void startPhase(float temp, float time) {
            phaseMax_ = phaseMin_ = temp;
            phaseMaxTime_ = phaseMinTime_ = time;
        }

        bool phaseStartedBySwitch_;
        unsigned sampleCount_;
        float lastSampleTime_;
        float phaseMax_;
        float phaseMin_;
        float phaseMaxTime_;
        float phaseMinTime_;
    };

} // namespace RelayExtrema

#endif // RELAY_EXTREMA_TRACKER_H
