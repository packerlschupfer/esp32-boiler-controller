/**
 * @file MockSystemSettings.h
 * @brief NOT A MOCK ANY MORE - it forwards to the real production struct.
 *
 * It WAS a hand-copied duplicate of SystemSettings whose constructor claimed "defaults matching
 * real SystemSettings". They had not matched for a long time (2026-10-25 review):
 *
 *   heating_curve_shift   mock 20.0  real 0.0   (changed 2026-09-15 with the 10x-too-weak curve)
 *   heating_curve_coeff   mock  2.0  real 1.4   (same change)
 *   heating_low_limit     mock 40.0  real 38.0  (changed 2026-10-19, the mild-weather clamp fix)
 *
 * A 20 degree curve shift is not a detail - any test reading these was asserting against a
 * boiler that has not existed since September, while reporting as coverage of one that does.
 * heating_boost_min_target was simply missing.
 *
 * The real SystemSettingsStruct.h includes only shared/Temperature.h and
 * BurnerHysteresisPolicy.h - no Arduino, no FreeRTOS - so there was never anything to mock. It
 * compiles natively as-is, and using it means the defaults cannot drift again rather than being
 * re-synced by hand after the next change.
 *
 * Kept as a forwarding header rather than deleted so the five includers keep working and the
 * reason stays where someone looking for the mock will find it.
 */

#ifndef MOCK_SYSTEM_SETTINGS_H
#define MOCK_SYSTEM_SETTINGS_H

#include "config/SystemSettingsStruct.h"

#endif // MOCK_SYSTEM_SETTINGS_H
