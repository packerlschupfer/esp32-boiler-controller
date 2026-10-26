// src/config/SystemSettings.cpp
#include "SystemSettings.h"
#include "shared/Temperature.h"

// Define the global currentSettings instance
SystemSettings currentSettings = getDefaultSystemSettings();

SystemSettings getDefaultSystemSettings() {
    // Default-construct, so the struct's own constructor defaults are the SINGLE definition.
    //
    // This used to be a 21-argument positional list restating every value, which made it a
    // second definition of all of them - and they had already drifted apart on one:
    // `heating_low_limit` was 40.0 here against 38.0 in the struct. That gave the value THREE
    // definitions (40.0 here, 38.0 in the constructor, 380 in the parameter registration), and
    // the registration wins post-load, so the 40.0 was dead weight that only applied to a
    // factory-fresh device between boot and the first NVS load. Found in the 2026-10-23 review;
    // collapsing the list rather than correcting the number removes the whole class.
    //
    // Verified before collapsing: the positional list and the constructor defaults agreed on
    // every other field, and the list already omitted the last four parameters
    // (useBoilerTempPID, autotuneRelayAmplitude, autotuneHysteresis, autotuneMethod) and so was
    // already relying on the constructor for those.
    return SystemSettings{};
}
