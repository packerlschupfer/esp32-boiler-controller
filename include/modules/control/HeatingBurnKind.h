// include/modules/control/HeatingBurnKind.h
#ifndef HEATING_BURN_KIND_H
#define HEATING_BURN_KIND_H

#include <atomic>

/**
 * @brief Is the current space-heating burn authorised by NOTHING BUT the frost latch?
 *
 * ONE DEFINITION, READ BY EVERYONE. This exists because the answer was being computed
 * independently in three places from different inputs, and the 2026-10-28 behavioural review
 * traced five findings - two of them high - to exactly that:
 *
 *   1. HeatingControlTask's TURN-ON path (correct): frost && !(switch && !override && wanted)
 *   2. HeatingControlTask's REFRESH path (wrong): frost && !wanted - it dropped the switch and
 *      override terms, so a frost burn ran at the flow floor for one 5 s cycle and then at the
 *      full weather curve, up to h/sHi, for the rest of the burn. The comment above it claimed
 *      "same floor substitution as the turn-on path". The 2026-10-26 fix changed the turn-on
 *      gate and not its sibling thirteen lines later.
 *   3. BurnerControlTask::operationModeForRequests (wrong, and the expensive one):
 *      !switch && frost - it dropped the `wanted` term entirely. Its comment also claimed
 *      "same discriminator as the frost FLOOR decision in HeatingControlTask".
 *
 * Why (3) mattered: with the heating switch ON and the room sensor dead, the heating task
 * classified the burn frost-only (the room is not a measurement, so `wanted` is false) while
 * the burner task classified it SPACE_HEATING - whose required-sensor set includes the room.
 * The sensor fallback then reached SHUTDOWN a few seconds after every start, the heating task
 * dropped the request, the mode reverted to NONE, the fallback recovered on one pass, and the
 * still-cold latch restarted it. An ignition roughly every 45-55 s for as long as the flow sat
 * between the 5 C trigger and the 10 C release, with the retained fallback topic flapping
 * NORMAL/SHUTDOWN each cycle.
 *
 * The lesson the review drew, which is why this is a module and not a fourth copy: in this
 * codebase "what kind of burn is this" had five independent predicates, and EVERY recent
 * regression came from changing one and not its siblings. A shared predicate cannot drift; two
 * copies with a comment asserting they match already have.
 */
namespace HeatingBurnKind {

    /**
     * @brief The predicate. Pure, so the native tests can pin every combination.
     *
     * @param frostLatched       FrostProtection::demand()
     * @param heatingSwitchOn    SystemState::HEATING_ENABLED
     * @param heatingOverrideOff ControlRequest::HEATING_OFF_OVERRIDE (summer mode)
     * @param spaceHeatingWanted checkIfSpaceHeatingNeededEvent() - the weather/room decision,
     *                           which knows nothing about the switch
     *
     * "Frost-only" means the latch is asking AND ordinary space heating is not. All three of
     * the other terms must be considered: with the switch off, or summer override on, or the
     * weather/room decision saying no, this burn exists only because of frost - and it must
     * therefore heat to the flow FLOOR and require only the boiler-output reading.
     */
    inline bool isFrostOnlyBurn(bool frostLatched, bool heatingSwitchOn,
                                bool heatingOverrideOff, bool spaceHeatingWanted) {
        const bool normalHeatingWanted =
            heatingSwitchOn && !heatingOverrideOff && spaceHeatingWanted;
        return frostLatched && !normalHeatingWanted;
    }

    /**
     * @brief The published answer for the burn currently requested.
     *
     * Written by HeatingControlTask, the only task that can decide it - it owns the
     * weather/room evaluation. Read by BurnerControlTask, which must not re-derive it: the
     * burner task cannot see `spaceHeatingWanted` without duplicating the whole heating
     * decision, which is precisely how (3) above went wrong.
     *
     * An atomic rather than a mutex: one bool, and a reader that sees the previous value for a
     * cycle loses nothing, because it is rewritten on every request issue and refresh. Cleared
     * when heating stops, so a stale `true` cannot outlive its burn and weaken the sensor
     * requirements of the NEXT one.
     */
    inline std::atomic<bool>& state() {
        static std::atomic<bool> s_frostOnly{false};
        return s_frostOnly;
    }

    inline void publish(bool frostOnly) {
        state().store(frostOnly, std::memory_order_relaxed);
    }

    inline bool isFrostOnly() {
        return state().load(std::memory_order_relaxed);
    }

    /// Call when the heating request is dropped, so the classification cannot outlive the burn.
    inline void clear() {
        state().store(false, std::memory_order_relaxed);
    }

    /**
     * @brief Is the room value the heating task is controlling on a real MEASUREMENT?
     *
     * The same one-definition problem, one layer down. BurnerTransitionPolicy's room rule was
     * fed `readings.isInsideTempValid` - the RAW flag - while the heating task decides on the
     * RESOLVED room from RoomTempFallbackPolicy, which holds a reading for up to 10 minutes
     * after the flag goes false. So the two disagreed in opposite directions at different
     * times, and neither flipping the raw flag nor ignoring it would have matched:
     *
     *   - 0 to ~15 s of failed reads: flag still TRUE, and the heating task agrees.
     *   - ~15 s to 10 min: flag FALSE but the hold is live, so the heating task STILL wants
     *     heat. Treating the raw flag as "no room" here would make the burner stop switching
     *     when it should wait - the opposite error.
     *   - past 10 min: hold expired, heating task returns ROOM_UNKNOWN and will never request.
     *     This is the case the burner got wrong: it waited 15 s with the burner still firing in
     *     water mode for a request that could not come.
     *
     * So the burner must be told, not left to guess. Published by the heating task, which owns
     * the hold. Defaults to true so that before the first evaluation the burner behaves as it
     * always did rather than refusing a mode switch on a fact nobody has established yet.
     */
    inline std::atomic<bool>& roomMeasurementState() {
        static std::atomic<bool> s_roomIsMeasurement{true};
        return s_roomIsMeasurement;
    }

    inline void publishRoomIsMeasurement(bool isMeasurement) {
        roomMeasurementState().store(isMeasurement, std::memory_order_relaxed);
    }

    inline bool roomIsMeasurement() {
        return roomMeasurementState().load(std::memory_order_relaxed);
    }

}  // namespace HeatingBurnKind

#endif  // HEATING_BURN_KIND_H
