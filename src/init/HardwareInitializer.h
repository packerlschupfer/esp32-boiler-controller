// src/init/HardwareInitializer.h
#ifndef HARDWARE_INITIALIZER_H
#define HARDWARE_INITIALIZER_H

#include "utils/ErrorHandler.h"

// Forward declarations
class SystemInitializer;
class DS3231Controller;
namespace rtstorage { class RuntimeStorage; }

/**
 * @brief Handles hardware initialization
 *
 * Initializes:
 * - RS485/Modbus serial communication
 * - DS3231 RTC (I2C)
 * - RuntimeStorage (FRAM)
 * - System timezone
 */
class HardwareInitializer {
    friend class SystemInitializer;

public:
    /**
     * @brief Initialize all hardware interfaces
     * @param initializer Parent SystemInitializer to store device pointers
     * @return Result indicating success or failure
     */
    static Result<void> initialize(SystemInitializer* initializer);

private:
    // Prevent instantiation
    HardwareInitializer() = delete;

    /**
     * @brief Initialize RS485 serial and Modbus master
     * @return Result indicating success or failure
     */
    static Result<void> initializeModbus();

    /**
     * @brief Initialize DS3231 RTC
     * @param ds3231 Output pointer for created DS3231Controller
     * @return true if successful (ds3231 may be null if RTC not found)
     */
    static bool initializeRTC(DS3231Controller*& ds3231);

    /**
     * @brief Initialize RuntimeStorage (FRAM)
     * @param storage Output pointer for created RuntimeStorage
     * @return true if successful (storage may be null if FRAM not found)
     */
    static bool initializeFRAM(rtstorage::RuntimeStorage*& storage);

    /**
     * @brief Set system timezone
     */
    static void initializeTimezone();
};

#endif // HARDWARE_INITIALIZER_H
