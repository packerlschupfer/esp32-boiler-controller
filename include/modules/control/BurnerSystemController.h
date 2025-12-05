// include/modules/control/BurnerSystemController.h
#pragma once

#include "modules/control/PumpCoordinator.h"
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
 * Unified controller that owns the complete burner subsystem:
 * - Burner relays (3: BURNER_ENABLE, 4: POWER_SELECT, 5: WATER_MODE)
 * - Pump relays (1: HEATING_PUMP, 2: WATER_PUMP)
 *
 * Eliminates coordination complexity by providing atomic operations:
 * - activateHeatingMode(): Sets heating pump + burner relays in one batch
 * - activateWaterMode(): Sets water pump + burner relays in one batch
 * - deactivate(): Turns off burner (keeps pumps running for safety)
 *
 * Features:
 * - Atomic relay changes via RYN4 batch commands
 * - 30s pump motor protection via PumpCoordinator
 * - Make-before-break mode switching
 * - Thread-safe operation
 * - Clear error reporting via Result<void>
 *
 * Usage:
 *   BurnerSystemController controller;
 *   controller.initialize();
 *
 *   // Activate water heating
 *   auto result = controller.activateWaterMode(tempFromWhole(30), PowerLevel::HIGH);
 *   if (result.isError()) {
 *       LOG_ERROR(TAG, "Failed: %s", result.getErrorMessage());
 *   }
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
     * @brief Activate heating mode
     *
     * Atomic operation:
     * 1. Check 30s pump protection
     * 2. Send batch command: heating pump ON, burner relays for heating mode
     * 3. Record pump state change
     *
     * @param targetTemp Target boiler temperature
     * @param power Power level (LOW/HIGH/AUTO)
     * @return Success if activated, error otherwise
     *
     * Errors:
     * - PUMP_PROTECTION_ACTIVE: 30s minimum not elapsed
     * - RELAY_OPERATION_FAILED: Batch command failed
     */
    Result<void> activateHeatingMode(Temperature_t targetTemp, PowerLevel power = PowerLevel::AUTO);

    /**
     * @brief Activate water heating mode
     *
     * Atomic operation:
     * 1. Check 30s pump protection
     * 2. Send batch command: water pump ON, burner relays for water mode
     * 3. Record pump state change
     *
     * @param targetTemp Target boiler temperature
     * @param power Power level (LOW/HIGH/AUTO)
     * @return Success if activated, error otherwise
     *
     * Errors:
     * - PUMP_PROTECTION_ACTIVE: 30s minimum not elapsed
     * - RELAY_OPERATION_FAILED: Batch command failed
     */
    Result<void> activateWaterMode(Temperature_t targetTemp, PowerLevel power = PowerLevel::AUTO);

    /**
     * @brief Deactivate burner
     *
     * Turns off burner relays (BURNER_ENABLE, WATER_MODE, POWER_SELECT).
     * Keeps pumps running for heat dissipation (safety).
     *
     * @return Success if deactivated
     */
    Result<void> deactivate();

    /**
     * @brief Emergency shutdown
     *
     * Immediate shutdown of burner, keeps pumps ON for heat dissipation.
     * Bypasses all checks and protection.
     *
     * @param reason Reason for emergency shutdown
     * @return Success (best-effort, always succeeds)
     */
    Result<void> emergencyShutdown(const char* reason = "Emergency shutdown");

    /**
     * @brief Switch to heat recovery mode
     *
     * Turns off burner and switches water pump → heating pump to use
     * residual boiler heat for space heating (energy savings).
     *
     * Use case: After water heating completes, if heating is enabled
     * and room needs heat, recover residual boiler heat.
     *
     * Conditions checked:
     * - Boiler output temp ≥35°C (minimum useful heat)
     * - Boiler differential ≥10°C (effective circulation)
     * - Heating pump 30s protection elapsed
     *
     * @return Success if switched, error if conditions not met
     *
     * Errors:
     * - PUMP_PROTECTION_ACTIVE: Heating pump 30s not elapsed
     * - PRECONDITION_FAILED: Boiler temp too low or differential insufficient
     */
    Result<void> switchToHeatRecovery();

    /**
     * @brief Check if burner activation is allowed during heat recovery
     *
     * During heat recovery mode, this checks if residual heat is exhausted
     * before allowing burner activation.
     *
     * Returns true if:
     * - Not in heat recovery mode, OR
     * - Heat recovery active for ≥2 minutes, OR
     * - Boiler output temp <40°C (residual heat exhausted)
     *
     * @return true if burner activation allowed
     */
    bool shouldActivateHeatingBurner() const;

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

    /**
     * @brief Check and handle pump cooldown
     *
     * Called periodically by BurnerControlTask. After burner deactivates,
     * pumps continue running for heat dissipation. This method checks if
     * the cooldown period has elapsed and turns off pumps.
     *
     * @return true if pumps were turned off (cooldown complete)
     */
    bool checkAndHandleCooldown();

    /**
     * @brief Check if in cooldown state
     */
    bool isInCooldown() const;

private:
    PumpCoordinator pumpCoordinator_;

    // Current state
    BurnerMode currentMode_;
    PowerLevel currentPower_;
    Temperature_t currentTarget_;
    bool isActive_;

    // Heat recovery mode state
    bool inHeatRecoveryMode_;
    uint32_t heatRecoveryStartTime_;

    // Pump cooldown state (after burner stops, pumps continue for heat dissipation)
    bool inCooldownMode_;
    uint32_t cooldownStartTime_;
    bool cooldownHeatingPumpWasOn_;
    bool cooldownWaterPumpWasOn_;

    // Thread safety
    mutable SemaphoreHandle_t mutex_;

    /**
     * @brief Build relay state array for batch command
     *
     * Constructs 8-relay state vector for RYN4 setMultipleRelays().
     *
     * @param mode Burner mode (HEATING/WATER/OFF)
     * @param power Power level (LOW/HIGH)
     * @param heatingPump Heating pump state
     * @param waterPump Water pump state
     * @return Vector of 8 relay states
     */
    std::array<bool, 8> buildRelayStates(BurnerMode mode, PowerLevel power,
                                         bool heatingPump, bool waterPump);

    /**
     * @brief Execute atomic relay batch command
     *
     * Sends all 8 relay states in single Modbus transaction.
     *
     * @param states 8 relay states
     * @return Success if batch command succeeded
     */
    Result<void> executeRelayBatch(const std::array<bool, 8>& states);
};
