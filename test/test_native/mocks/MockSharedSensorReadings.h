/**
 * @file MockSharedSensorReadings.h
 * @brief NOT A MOCK ANY MORE - it forwards to the real production struct.
 *
 * It WAS a hand-copied duplicate of SharedSensorReadings, and it had drifted: the 2026-10-25
 * review found it was missing `insideTempGoodMs`, added 2026-10-23 for the room-temperature
 * HOLD. That absence was not cosmetic - it is exactly why a native test of
 * HeatingControlModule::resolveRoomForControl() "could not be written", and that resolver is
 * the thing standing between a pulled room sensor and the burner.
 *
 * The real struct had no business being unmockable: it has no FreeRTOS members (access goes
 * through SRP). Its header just carried two unused FreeRTOS includes and an unused
 * ProjectConfig.h, and ProjectConfig pulls in <Arduino.h>. Removing those three - plus swapping
 * <Arduino.h> for <cstdint>/<cstdio> in Pressure.h, which only needed int16_t, INT16_MIN and
 * snprintf - makes it compile natively as-is.
 *
 * So the tests now use the real struct, which cannot drift, instead of a copy that had to be
 * re-synced by hand after every change to the real one and twice was not.
 *
 * Kept as a forwarding header rather than deleted so the existing includers keep working and
 * the reason stays where someone looking for the mock will find it.
 */

#ifndef MOCK_SHARED_SENSOR_READINGS_H
#define MOCK_SHARED_SENSOR_READINGS_H

#include "shared/SharedSensorReadings.h"

#endif // MOCK_SHARED_SENSOR_READINGS_H
