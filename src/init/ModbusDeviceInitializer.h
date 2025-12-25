// src/init/ModbusDeviceInitializer.h
#pragma once

#include "utils/ErrorHandler.h"

// Forward declarations
class SystemInitializer;
class MB8ART;
class RYN4;
namespace andrtf3 { class ANDRTF3; }
class DS3231Controller;
namespace RuntimeStorage { class RuntimeStorage; }

/**
 * @brief Handles initialization of Modbus devices (MB8ART, RYN4, ANDRTF3, RuntimeStorage)
 *
 * This class is a friend of SystemInitializer and handles the complex device
 * initialization logic including retry handling and background monitoring.
 */
class ModbusDeviceInitializer {
public:
    /**
     * @brief Initialize all Modbus devices
     * @param initializer Pointer to the SystemInitializer instance
     * @return Result indicating success or failure
     */
    static Result<void> initializeDevices(SystemInitializer* initializer);

private:
    /**
     * @brief Initialize MB8ART temperature sensor
     * @param initializer Pointer to the SystemInitializer instance
     * @param outInitialized Set to true if device was successfully initialized
     * @return Result indicating success or failure
     */
    static Result<void> initializeMB8ART(SystemInitializer* initializer, bool& outInitialized);

    /**
     * @brief Initialize ANDRTF3 room temperature sensor
     * @param initializer Pointer to the SystemInitializer instance
     * @param outInitialized Set to true if device was successfully initialized
     */
    static void initializeANDRTF3(SystemInitializer* initializer, bool& outInitialized);

    /**
     * @brief Initialize RYN4 relay controller
     * @param initializer Pointer to the SystemInitializer instance
     * @param outInitialized Set to true if device was successfully initialized
     * @return Result indicating success or failure
     */
    static Result<void> initializeRYN4(SystemInitializer* initializer, bool& outInitialized);

    /**
     * @brief Initialize CriticalDataStorage (uses RuntimeStorage which is initialized earlier)
     * @param initializer Pointer to the SystemInitializer instance
     */
    static void initializeCriticalDataStorage(SystemInitializer* initializer);

    /**
     * @brief Create background device monitoring task
     * @param initializer Pointer to the SystemInitializer instance
     * @param mb8artInitialized Whether MB8ART was initialized
     * @param ryn4Initialized Whether RYN4 was initialized
     */
    static void createBackgroundMonitoringTask(SystemInitializer* initializer,
                                                bool mb8artInitialized,
                                                bool ryn4Initialized);
};
