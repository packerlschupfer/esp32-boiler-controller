// src/hal/ANDRTF3TemperatureSensor.h
// HAL interface for ANDRTF3 temperature sensor

#pragma once

#include "hal/HardwareAbstractionLayer.h"
#include "modules/control/RoomSensorRegisterReport.h"
#include <ANDRTF3.h>

namespace HAL {

/**
 * @brief ANDRTF3 temperature sensor HAL implementation
 * 
 * This HAL wrapper provides a consistent interface for the ANDRTF3
 * wall-mount RS485 temperature sensor. The sensor provides temperature
 * readings in fixed-point format (value * 10).
 */
class ANDRTF3TemperatureSensor : public ITemperatureSensor {
private:
    andrtf3::ANDRTF3* device;
    bool initialized;
    static constexpr const char* TAG = "ANDRTF3HAL";
    
public:
    /**
     * @brief Construct ANDRTF3 temperature sensor HAL
     * @param andrtf3Device Pointer to ANDRTF3 device instance
     */
    explicit ANDRTF3TemperatureSensor(andrtf3::ANDRTF3* andrtf3Device);
    
    /**
     * @brief Initialize the sensor
     * @return true if successful
     */
    bool initialize() override;
    
    /**
     * @brief Read temperature from sensor
     * @param channel Sensor channel (ignored for ANDRTF3 - single channel)
     * @return Temperature reading with validity flag
     */
    Reading readTemperature(uint8_t channel = 0) override;
    
    /**
     * @brief Get number of available channels
     * @return Always returns 1 for ANDRTF3
     */
    uint8_t getChannelCount() const override;
    
    /**
     * @brief Check if sensor is ready
     * @return true if sensor is initialized and connected
     */
    bool isReady() const override;
    
    /**
     * @brief Get sensor name
     * @return "ANDRTF3"
     */
    const char* getName() const override;
    
private:
    /**
     * @brief Request asynchronous temperature reading
     * @return true if request was sent successfully
     */
    bool requestAsyncReading();
    
};

/**
 * @brief Read-path statistics for the one room sensor, for publishing.
 *
 * Exists because the ANDRTF3's failure counters live on the library object, which only this
 * translation unit holds: the tasks reach the sensor through ITemperatureSensor*, and these
 * counters are specific to this device (the MB8ART channels have no equivalent), so widening
 * that interface for them would be wrong.
 *
 * `counts` is indexed by andrtf3::ANDRTF3::ReadError, where index 0 (NONE) counts SUCCESSFUL
 * reads - that is the denominator for a failure rate.
 */
struct RoomSensorStats {
    static constexpr size_t KIND_COUNT =
        static_cast<size_t>(andrtf3::ANDRTF3::ReadError::COUNT);
    uint32_t counts[KIND_COUNT] = {0};
    uint32_t lastGoodAgeMs = UINT32_MAX;  // UINT32_MAX = never read successfully
    uint8_t lastKind = 0;                 // andrtf3::ANDRTF3::ReadError of the last read
    bool available = false;               // false if no ANDRTF3 device was ever created
};

/**
 * @brief Snapshot the room sensor's read statistics.
 *
 * Each counter is an aligned 32-bit word so individual reads cannot tear, but this sweep is
 * NOT a coherent snapshot: a read completing mid-sweep means the counts need not sum to the
 * total number of reads. That is fine for diagnostics and must not be relied on for more.
 */
RoomSensorStats getRoomSensorStats();

/**
 * @brief Read the sensor's OWN counters - input registers 50..77 of the open firmware.
 *
 * One FC04 transaction through the library's normal bus mutex, so it is safe only from a task
 * that already owns the Modbus client path: call it from ANDRTF3Task, not from the MQTT task.
 * Needs no bus-release window - it is an ordinary request to an ordinary slave.
 *
 * Runs `detectFirmware()` once on first use if the firmware is still UNKNOWN, so the report can
 * say WHICH sensor firmware answered. That matters: attributing an error rate to a firmware
 * build is impossible without it, and this project had to ask elsewhere for the install
 * timeline once already.
 *
 * On-demand only, never on a cadence. The counters are cumulative since the SENSOR's last boot,
 * so one read gives the whole period - polling it buys nothing and costs a bus slot.
 *
 * @return a snapshot whose `available` / `ok` flags say whether the numbers mean anything
 */
RoomSensorRegisterReport::Snapshot readRoomSensorDiagnostics();

/**
 * @brief Factory function to create ANDRTF3 temperature sensor HAL
 * @param device ANDRTF3 device instance
 * @return New ANDRTF3TemperatureSensor instance
 */
ITemperatureSensor* createANDRTF3Sensor(andrtf3::ANDRTF3* device);

} // namespace HAL