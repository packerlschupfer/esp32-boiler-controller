// src/core/ModbusBusRelease.h
#ifndef MODBUS_BUS_RELEASE_H
#define MODBUS_BUS_RELEASE_H

#include <cstdint>

#include "modules/control/BusReleasePolicy.h"

/**
 * @brief Runtime state for a deliberate release of the RS485 bus to another master.
 *
 * See BusReleasePolicy.h for why this exists and why it is shaped this way. This header is
 * only the state: one mutator and several cheap readers.
 *
 * THE MUTATOR/READER SPLIT IS DELIBERATE. expireIfDue() is the only function that ends a
 * window on its own, and it is called from ModbusCoordinator::processTick() - the one place
 * that can grant bus access - plus the periodic publisher as a backstop. Everything else
 * (notably BurnerStateMachine::setHeatDemand, which runs under demandMutex) uses isReleased(),
 * a pure atomic read. An earlier draft expired the window inside isReleased(); that put a
 * relay-mask mutation inside the demand-write path, which is a surprising place to find one.
 */
namespace ModbusBusRelease {

    /**
     * @brief Hand the bus over for `seconds` (clamped by BusReleasePolicy).
     * @return Refusal::NONE on success; otherwise nothing changed.
     *
     * Refuses unless the burner is off and no demand is armed - this does not stop the
     * boiler for you, by design.
     */
    BusReleasePolicy::Refusal release(uint32_t seconds);

    /// End the window now. Safe to call when not released.
    void acquire();

    /// Pure read. True while the bus belongs to someone else.
    bool isReleased();

    /// End the window if its deadline has passed. Call from the bus scheduler.
    void expireIfDue();

    /// Seconds remaining, 0 when not released.
    uint32_t secondsLeft();

    /// How the window ended, for the status topic.
    enum class Ended : uint8_t { NOT_YET, BY_COMMAND, BY_TIMEOUT };

    /// True once per transition, so a periodic publisher can report changes without
    /// publishing the same state forever. Clears the flag.
    bool consumeStateChange();

    /// Why the last window ended.
    Ended lastEnd();

} // namespace ModbusBusRelease

#endif // MODBUS_BUS_RELEASE_H
