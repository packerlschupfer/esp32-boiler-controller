#include "RelayBindings.h"
#include "config/RelayIndices.h"
#include "SharedRelayReadings.h"
#include "core/SystemResourceProvider.h"

namespace RelayBindings {

    std::array<bool*, RelayIndex::MAX_RELAYS> pointers = {};

    void initialize() {
        auto& readings = SRP::getRelayReadings();

        // Bind logical functions to their state variables
        // This is the ONLY place that connects RelayIndex to SharedRelayReadings
        pointers[RelayIndex::HEATING_PUMP]  = &readings.relayHeatingPump;
        pointers[RelayIndex::WATER_PUMP]    = &readings.relayWaterPump;
        pointers[RelayIndex::BURNER_ENABLE] = &readings.relayBurnerEnable;
        pointers[RelayIndex::POWER_BOOST]   = &readings.relayPowerBoost;
        pointers[RelayIndex::WATER_MODE]    = &readings.relayWaterMode;
        pointers[RelayIndex::VALVE]         = &readings.relayValve;
        pointers[RelayIndex::SPARE_7]       = nullptr;
        pointers[RelayIndex::ALARM]         = nullptr;
    }
}
