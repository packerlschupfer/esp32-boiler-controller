// src/diagnostics/DiagnosticsRecoveryTimer.h
#ifndef DIAGNOSTICS_RECOVERY_TIMER_H
#define DIAGNOSTICS_RECOVERY_TIMER_H

#include <cstdint>

/**
 * @brief Schedule diagnostics restoration after emergency memory recovery
 * 
 * This creates a one-shot timer that will restore normal MQTT diagnostic
 * operation after the specified delay. Used after emergency memory
 * recovery to give the system time to stabilize.
 * 
 * @param delayMs Delay in milliseconds before restoration (default 30000)
 * @return true if timer was created successfully
 */
bool scheduleDiagnosticsRecovery(uint32_t delayMs = 30000);

#endif // DIAGNOSTICS_RECOVERY_TIMER_H