// src/hal/ANDRTF3TemperatureSensor.cpp
// HAL implementation for ANDRTF3 temperature sensor

#include "hal/ANDRTF3TemperatureSensor.h"
#include "core/SystemResourceProvider.h"
#include "config/SystemConstants.h"
#include "utils/Utils.h"
#include <TaskManager.h>
#include "LoggingMacros.h"
#include <Arduino.h>

namespace HAL {

ANDRTF3TemperatureSensor::ANDRTF3TemperatureSensor(andrtf3::ANDRTF3* andrtf3Device) 
    : device(andrtf3Device), initialized(false) {
}

bool ANDRTF3TemperatureSensor::initialize() {
    // Guard against double initialization
    if (initialized) {
        return true;
    }

    if (!device) {
        LOG_ERROR(TAG, "Device pointer is null");
        return false;
    }

    // ANDRTF3 doesn't have an explicit initialization method
    // Just check if device is ready for communication
    initialized = true;
    LOG_INFO(TAG, "ANDRTF3 HAL initialized for address %u", device->getDeviceAddress());

    return true;
}

// The one ANDRTF3 device, remembered by createANDRTF3Sensor() so getRoomSensorStats() can
// reach the library's counters. There is exactly one room sensor in this system, and the
// tasks hold only an ITemperatureSensor*, which deliberately knows nothing about them.
static andrtf3::ANDRTF3* g_roomSensorDevice = nullptr;

ITemperatureSensor::Reading ANDRTF3TemperatureSensor::readTemperature(uint8_t channel) {
    Reading result = {0.0f, false, 0};
    
    // ANDRTF3 is single-channel, ignore channel parameter
    if (channel > 0) {
        LOG_WARN(TAG, "Channel %u requested but ANDRTF3 only has 1 channel", channel);
    }
    
    if (!initialized || !device) {
        LOG_ERROR(TAG, "Sensor not initialized");
        return result;
    }
    
    // The library's "async" trio is synchronous - confirmed with its maintainer 2026-10-24
    // and now documented as such upstream: requestTemperature() performs the whole
    // transaction, isReadComplete() always returns true, and getAsyncResult() hands back the
    // stored result. So there is nothing to wait for.
    //
    // This used to spin a processAsyncReading() loop with a 250 ms "timeout to allow for
    // Modbus bus contention (library uses 200ms internally)". Both halves were wrong: the
    // loop condition `!device->isReadComplete()` is false immediately, so its body never ran,
    // and the library's bus-mutex timeout is MODBUS_MUTEX_TIMEOUT_MS (2000 ms). The loop and
    // its timeout are gone rather than left looking load-bearing.
    //
    // Why not make it genuinely async: RS485 is half-duplex, so the bus mutex is held for
    // request AND response however the call is shaped. Async would only stop THIS task from
    // blocking - it would not reduce bus occupancy, which was the reason I asked for it.
    if (requestAsyncReading()) {
        andrtf3::ANDRTF3::TemperatureData tempData;
        if (device->getAsyncResult(tempData) && tempData.valid) {
            // Convert from fixed-point format (value * 10) to float
            result.temperature = tempData.celsius / 10.0f;
            result.valid = true;
            result.timestamp = tempData.timestamp;  // millis() of this good read

            LOG_DEBUG(TAG, "Temperature: %.1f°C", result.temperature);
        } else {
            // errorKind is the machine-readable form; the string is kept for the log line.
            LOG_WARN(TAG, "Read failed: %s (kind %u)", tempData.error.c_str(),
                     static_cast<unsigned>(tempData.errorKind));
        }
    }
    // No retry - if read failed, wait for next coordinator tick (5s)

    return result;
}

uint8_t ANDRTF3TemperatureSensor::getChannelCount() const {
    return 1;  // ANDRTF3 is a single-channel sensor
}

bool ANDRTF3TemperatureSensor::isReady() const {
    return initialized && device && device->isConnected();
}

const char* ANDRTF3TemperatureSensor::getName() const {
    return "ANDRTF3";
}

bool ANDRTF3TemperatureSensor::requestAsyncReading() {
    if (!device) {
        return false;
    }
    
    // Process any pending operations first
    device->process();
    
    // Check if a previous read is still pending
    if (!device->isReadComplete()) {
        LOG_DEBUG(TAG, "Previous async read still pending");
        return false;
    }
    
    // Request new temperature reading
    return device->requestTemperature();
}

/**
 * @brief Factory function to create ANDRTF3 temperature sensor HAL
 */
ITemperatureSensor* createANDRTF3Sensor(andrtf3::ANDRTF3* device) {
    if (!device) {
        LOG_ERROR("ANDRTF3HAL", "Cannot create HAL with null device");
        return nullptr;
    }

    g_roomSensorDevice = device;
    return new ANDRTF3TemperatureSensor(device);
}

RoomSensorStats getRoomSensorStats() {
    RoomSensorStats stats;
    andrtf3::ANDRTF3* dev = g_roomSensorDevice;
    if (dev == nullptr) {
        return stats;  // available stays false
    }
    stats.available = true;
    for (size_t i = 0; i < RoomSensorStats::KIND_COUNT; ++i) {
        stats.counts[i] = dev->getReadErrorCount(static_cast<andrtf3::ANDRTF3::ReadError>(i));
    }
    stats.lastGoodAgeMs = dev->getLastGoodReadAgeMs();
    stats.lastKind = static_cast<uint8_t>(dev->getLastReadError());
    return stats;
}

// The register map our Snapshot assumes, pinned where both headers are in scope. If the
// library's decode order ever changes, this fails to compile instead of silently publishing
// the wrong counter under the right name - the failure mode that makes a diagnostic worse
// than no diagnostic. Same approach as the KIND_KEYS assert in ANDRTF3Task.cpp.
static_assert(andrtf3::DIAG_FIRST == 50, "Snapshot documents registers 50..77");
static_assert(andrtf3::DIAG_COUNT == 28, "Snapshot documents registers 50..77");

RoomSensorRegisterReport::Snapshot readRoomSensorDiagnostics() {
    RoomSensorRegisterReport::Snapshot snap;
    andrtf3::ANDRTF3* dev = g_roomSensorDevice;
    if (dev == nullptr) {
        return snap;  // available stays false
    }
    snap.available = true;

    // One-time detection, so the report can name the firmware that produced the counters.
    // readDiagnostics() would attempt the read anyway while the firmware is UNKNOWN, but then
    // `fw` would be 0 and the whole point is attributing numbers to a build.
    if (dev->getFirmware() == andrtf3::ANDRTF3::Firmware::UNKNOWN) {
        (void)dev->detectFirmware();
    }
    const andrtf3::ANDRTF3::Firmware fw = dev->getFirmware();
    snap.firmwareVersion = dev->getFirmwareVersion();
    snap.firmwareKnown = (fw != andrtf3::ANDRTF3::Firmware::UNKNOWN);
    snap.openFirmware = (fw == andrtf3::ANDRTF3::Firmware::OPEN);

    // A still-UNKNOWN firmware means the detection frame itself got no answer: the sensor is
    // absent, mid-reflash, or the RS485 pair is broken. Stop here rather than spending a
    // second ~1 s bus timeout on registers that cannot answer either - readDiagnostics()
    // only skips for ORIGINAL, so with UNKNOWN it would transact and time out again. That
    // second timeout is what let one diagnostics request hold the shared bus for ~3 s across
    // the RYN4 and MB8ART slots while the sensor was dead. Found 2026-10-28.
    if (!snap.firmwareKnown) {
        return snap;  // ok stays false; the formatter reports "no_answer"
    }

    andrtf3::Diagnostics d;
    if (!dev->readDiagnostics(d)) {
        return snap;  // ok stays false; the formatter says which of the three reasons it was
    }
    snap.ok = true;

    snap.uptimeSeconds     = d.uptimeSeconds;
    snap.goodReadings      = d.goodReadings;
    snap.failedReadings    = d.failedReadings;
    snap.lastFailReason    = d.lastFailReason;
    snap.adcTimeouts       = d.adcTimeouts;
    snap.rangeErrors       = d.rangeErrors;
    snap.adcOverflows      = d.adcOverflows;
    snap.secondsSinceGood  = d.secondsSinceGood;
    snap.status            = d.status;
    snap.resistance        = d.resistance;
    snap.temperatureCenti  = d.temperatureCenti;
    snap.busFrames         = d.busFrames;
    snap.busHandled        = d.busHandled;
    snap.busCrcErrors      = d.busCrcErrors;
    snap.busBadLength      = d.busBadLength;
    snap.busFramingErrors  = d.busFramingErrors;
    snap.busParityErrors   = d.busParityErrors;
    snap.busNoiseErrors    = d.busNoiseErrors;
    snap.busOverruns       = d.busOverruns;
    snap.busExceptions     = d.busExceptions;
    snap.resetCause        = d.resetCause;
    snap.runningSlot       = d.runningSlot;
    snap.bootloaderVersion = d.bootloaderVersion;
    return snap;
}

} // namespace HAL