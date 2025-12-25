// include/modules/control/BurnerSystemController.h
// Controls BURNER relays only (0,1,2). Pump relays (4,5) controlled by PumpControlModule.
#pragma once

#include "shared/Temperature.h"
#include "utils/ErrorHandler.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <array>

// Forward declarations (avoid circular dependencies)
class BurnerStateMachine;

/**
 * @brief Burner operating modes
 */
enum class BurnerMode {
    OFF,
    HEATING,
    WATER,
    BOTH
};

/**
 * @brief Power level for burner operation
 */
enum class PowerLevel : uint8_t {
    HALF = 0,     ///< Half power (POWER_SELECT = ON)
    FULL = 1,     ///< Full power (POWER_SELECT = OFF)
    AUTO = 2      ///< Automatic (determined by state machine)
};

/**
 * @brief Centralized Burner System Controller
 *
 * Controls BURNER relays only:
 * - Relay 0: BURNER_ENABLE (heating mode base)
 * - Relay 1: POWER_BOOST (full power for either mode)
 * - Relay 2: WATER_MODE (water heating mode)
 *
 * NOTE: Pump relays (4,5) are controlled independently by PumpControlModule.
 * This separation allows pumps to run while burner is off (coasting).
 *
 * Features:
 * - Atomic burner relay changes via RYN4
 * - Thread-safe operation
 * - Clear error reporting via Result<void>
 *
 * Usage:
 *   BurnerSystemController controller;
 *   controller.initialize();
 *
 *   // Activate heating mode (burner only - pump handled by PumpControlModule)
 *   auto result = controller.activateHeatingMode(tempFromWhole(55), PowerLevel::HALF);
 */
class BurnerSystemController {
public:
    /**
     * @brief Constructor
     */
    BurnerSystemController();

    /**
     * @brief Destructor
     */
    ~BurnerSystemController();

    /**
     * @brief Initialize the controller
     *
     * Must be called before any operations.
     *
     * @return Success if initialized
     */
    Result<void> initialize();

    /**
     * @brief Shutdown the controller
     */
    void shutdown();

    /**
     * @brief Activate heating mode (burner relays only)
     *
     * Sets burner relays for heating mode. Pump control is handled
     * independently by PumpControlModule based on HEATING_ON event bit.
     *
     * @param targetTemp Target boiler temperature
     * @param power Power level (HALF/FULL/AUTO)
     * @return Success if activated, error otherwise
     */
    Result<void> activateHeatingMode(Temperature_t targetTemp, PowerLevel power = PowerLevel::AUTO);

    /**
     * @brief Activate water heating mode (burner relays only)
     *
     * Sets burner relays for water mode. Pump control is handled
     * independently by PumpControlModule based on WATER_ON event bit.
     *
     * @param targetTemp Target boiler temperature
     * @param power Power level (HALF/FULL/AUTO)
     * @return Success if activated, error otherwise
     */
    Result<void> activateWaterMode(Temperature_t targetTemp, PowerLevel power = PowerLevel::AUTO);

    /**
     * @brief Seamlessly switch between HEATING ↔ WATER modes (burner relays only)
     *
     * Switches burner relays between heating and water mode without restart.
     * Pump switching is handled independently by PumpControlModule based on
     * HEATING_ON/WATER_ON event bits (set by HeatingControlModule/WheaterControlModule).
     *
     * @param newMode Target mode (HEATING or WATER only, not OFF)
     * @param targetTemp Target temperature for new mode
     * @return Success if mode switched, error otherwise
     */
    Result<void> switchMode(BurnerMode newMode, Temperature_t targetTemp);

    /**
     * @brief Deactivate burner (burner relays only)
     *
     * Turns off burner relays (BURNER_ENABLE, WATER_MODE, POWER_BOOST).
     * Pump control is independent - PumpControlModule will keep pumps running
     * as long as HEATING_ON/WATER_ON bits are set.
     *
     * @return Success if deactivated
     */
    Result<void> deactivate();

    /**
     * @brief Emergency shutdown
     *
     * Immediate shutdown of all relays including pumps (safety measure).
     * Bypasses all checks and protection.
     *
     * @param reason Reason for emergency shutdown
     * @return Success (best-effort, always succeeds)
     */
    Result<void> emergencyShutdown(const char* reason = "Emergency shutdown");

    /**
     * @brief Check if burner is currently active
     */
    bool isActive() const;

    /**
     * @brief Get current burner mode
     */
    BurnerMode getCurrentMode() const;

    /**
     * @brief Get current power level
     */
    PowerLevel getCurrentPowerLevel() const;

    /**
     * @brief Get current target temperature
     */
    Temperature_t getCurrentTargetTemp() const;

    /**
     * @brief Set power level (batch relay command)
     *
     * Changes burner power level without affecting pump state.
     * Used by BurnerStateMachine for LOW↔HIGH transitions.
     *
     * @param power Power level (HALF or FULL)
     * @return Success if power changed
     */
    Result<void> setPowerLevel(PowerLevel power);

    /**
     * @brief Perform safety check
     *
     * Delegates to BurnerControlModule::performSafetyCheck() for now.
     * TODO: Move safety logic here in future refactoring.
     *
     * @return Success if safe to operate
     */
    Result<void> performSafetyCheck();

private:
    // Current state
    BurnerMode currentMode_;
    PowerLevel currentPower_;
    Temperature_t currentTarget_;
    bool isActive_;

    // Thread safety
    mutable SemaphoreHandle_t mutex_;

    /**
     * @brief Build relay state array for burner relays only
     *
     * Constructs relay states for indices 0,1,2 (burner relays).
     * Pump relays (4,5) are NOT controlled here - see PumpControlModule.
     *
     * @param mode Burner mode (HEATING/WATER/OFF)
     * @param power Power level (HALF/FULL)
     * @return Array of 8 relay states (only 0,1,2 are set, others false)
     */
    std::array<bool, 8> buildRelayStates(BurnerMode mode, PowerLevel power);

    /**
     * @brief Execute atomic relay batch command
     *
     * Sends relay states in single Modbus transaction.
     *
     * @param states 8 relay states
     * @return Success if batch command succeeded
     */
    Result<void> executeRelayBatch(const std::array<bool, 8>& states);
};
