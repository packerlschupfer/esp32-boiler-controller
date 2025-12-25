#pragma once
#include <array>
#include <cstdint>
#include "base/BaseRelayMapping.h"
#include "ryn4/RelayDefs.h"
#include "config/RelayIndices.h"

namespace RelayHardware {

    /**
     * @brief Hardware configuration for relay module (constexpr - flash)
     *
     * This constexpr array defines the hardware configuration for all relays.
     * It lives in flash memory and is indexed by the RelayIndex constants.
     *
     * To change relay assignments, modify RelayIndices.h ONLY.
     */
    constexpr std::array<base::RelayHardwareConfig, RelayIndex::MAX_RELAYS> CONFIGS = {{
        // Index 0 - HEATING_PUMP
        {RelayIndex::toPhysical(0), ryn4::RELAY_ON_BITS[0], ryn4::RELAY_OFF_BITS[0],
         ryn4::RELAY_STATUS_BITS[0], ryn4::RELAY_UPDATE_BITS[0], ryn4::RELAY_ERROR_BITS[0], false},
        // Index 1 - WATER_PUMP
        {RelayIndex::toPhysical(1), ryn4::RELAY_ON_BITS[1], ryn4::RELAY_OFF_BITS[1],
         ryn4::RELAY_STATUS_BITS[1], ryn4::RELAY_UPDATE_BITS[1], ryn4::RELAY_ERROR_BITS[1], false},
        // Index 2 - BURNER_ENABLE
        {RelayIndex::toPhysical(2), ryn4::RELAY_ON_BITS[2], ryn4::RELAY_OFF_BITS[2],
         ryn4::RELAY_STATUS_BITS[2], ryn4::RELAY_UPDATE_BITS[2], ryn4::RELAY_ERROR_BITS[2], false},
        // Index 3 - POWER_BOOST
        {RelayIndex::toPhysical(3), ryn4::RELAY_ON_BITS[3], ryn4::RELAY_OFF_BITS[3],
         ryn4::RELAY_STATUS_BITS[3], ryn4::RELAY_UPDATE_BITS[3], ryn4::RELAY_ERROR_BITS[3], false},
        // Index 4 - WATER_MODE
        {RelayIndex::toPhysical(4), ryn4::RELAY_ON_BITS[4], ryn4::RELAY_OFF_BITS[4],
         ryn4::RELAY_STATUS_BITS[4], ryn4::RELAY_UPDATE_BITS[4], ryn4::RELAY_ERROR_BITS[4], false},
        // Index 5 - VALVE
        {RelayIndex::toPhysical(5), ryn4::RELAY_ON_BITS[5], ryn4::RELAY_OFF_BITS[5],
         ryn4::RELAY_STATUS_BITS[5], ryn4::RELAY_UPDATE_BITS[5], ryn4::RELAY_ERROR_BITS[5], false},
        // Index 6 - SPARE_7
        {RelayIndex::toPhysical(6), ryn4::RELAY_ON_BITS[6], ryn4::RELAY_OFF_BITS[6],
         ryn4::RELAY_STATUS_BITS[6], ryn4::RELAY_UPDATE_BITS[6], ryn4::RELAY_ERROR_BITS[6], false},
        // Index 7 - SPARE_8
        {RelayIndex::toPhysical(7), ryn4::RELAY_ON_BITS[7], ryn4::RELAY_OFF_BITS[7],
         ryn4::RELAY_STATUS_BITS[7], ryn4::RELAY_UPDATE_BITS[7], ryn4::RELAY_ERROR_BITS[7], false}
    }};

    // Helper to get config by logical index
    constexpr const base::RelayHardwareConfig& get(uint8_t index) {
        return CONFIGS[index];
    }
}
