// include/modules/control/FrostProtection.h
#ifndef FROST_PROTECTION_H
#define FROST_PROTECTION_H

/**
 * @brief The frost latch: evaluated once per control cycle, read by the heating path.
 *
 * WHY THIS IS A SEPARATE MODULE (2026-10-25)
 *
 * The frost demand was a `static bool g_frostDemand` inside HeatingControlTask.cpp. It moved
 * here when a second latch briefly existed for a tank-driven DHW charge, which needed a reader
 * in another translation unit. That second latch is gone (2026-10-26 - see
 * FrostProtectionPolicy::demand() for why), but the module stays: the burner state machine's
 * Environment also needs to read the latch, so a static in the heating task would not do.
 *
 * The evaluation stays in ONE place deliberately. Two readers each deciding "is it freezing"
 * from their own sensor snapshot would be two latches that could disagree about the same
 * instant, and the readings come from one shared struct anyway.
 *
 * The latch overrides the HEATING switch - being switched off is exactly when freezing happens
 * - and does NOT override BOILER_ENABLED. A drained-for-service system is a likely reason to
 * switch the boiler off, and firing into a drained boiler is worse than frost. See
 * FrostProtectionPolicy.h for the rest of the reasoning and for what this cannot cover.
 */
namespace FrostProtection {

    /// Recompute the latch from the shared sensor readings. Called from HeatingControlTask on
    /// every control cycle; other readers only read the result.
    void evaluate();

    /// Heating wanted because the boiler water, the house or the tank is near freezing.
    bool demand();

}  // namespace FrostProtection

#endif  // FROST_PROTECTION_H
