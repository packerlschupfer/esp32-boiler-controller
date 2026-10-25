// include/modules/control/FrostProtection.h
#ifndef FROST_PROTECTION_H
#define FROST_PROTECTION_H

/**
 * @brief The two frost latches, evaluated once and read by both mode tasks.
 *
 * WHY THIS EXISTS (2026-10-25)
 *
 * The frost demand used to be a `static bool g_frostDemand` inside HeatingControlTask.cpp,
 * which was fine while heating was the only response. It no longer is: a cold TANK needs a
 * water charge, and the water task cannot see a static in another translation unit.
 *
 * The evaluation stays in one place on purpose. Two tasks each deciding "is it freezing" from
 * their own sensor snapshot would be two latches that could disagree about the same instant,
 * and the readings come from one shared struct anyway.
 *
 * Both latches override their own mode switch - heating frost overrides HEATING_ENABLED, water
 * frost overrides WATER_ENABLED, because being switched off is exactly when freezing happens -
 * and NEITHER overrides BOILER_ENABLED. A drained-for-service system is a likely reason to
 * switch the boiler off, and firing into a drained boiler is worse than frost. See
 * FrostProtectionPolicy.h for the rest of the reasoning and for what this cannot cover.
 */
namespace FrostProtection {

    /// Recompute both latches from the shared sensor readings. Called from HeatingControlTask,
    /// which runs it on every control cycle; the water task only reads the result.
    void evaluate();

    /// Space heating wanted because the boiler water or the house is near freezing.
    bool heatDemand();

    /// A DHW charge wanted because the tank is near freezing.
    bool waterDemand();

}  // namespace FrostProtection

#endif  // FROST_PROTECTION_H
