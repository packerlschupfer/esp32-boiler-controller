// src/init/HardwareInitializer.cpp
#include "HardwareInitializer.h"
#include "SystemInitializer.h"

#include <Arduino.h>
#include <Wire.h>
#include <time.h>
#include <sys/time.h>
#include <esp_log.h>
#include <esp32ModbusRTU.h>
#include <ModbusRegistry.h>
#include <ModbusDevice.h>
#include <RuntimeStorage.h>

#include "LoggingMacros.h"
#include "config/ProjectConfig.h"
#include "core/SystemResourceProvider.h"
#include "shared/SharedI2CInitializer.h"
#include "DS3231Controller.h"


static const char* TAG = "HardwareInitializer";
// External globals
extern rtstorage::RuntimeStorage* gRuntimeStorage;

Result<void> HardwareInitializer::initialize(SystemInitializer* initializer) {
    LOG_INFO(TAG, "Initializing hardware interfaces...");

    // Initialize RS485/Modbus
    auto result = initializeModbus();
    if (result.isError()) {
        return result;
    }

    // Set system timezone before any time operations
    initializeTimezone();

    // Initialize DS3231 RTC
    initializeRTC(initializer->ds3231_);

    // Initialize RuntimeStorage (FRAM)
    initializeFRAM(initializer->runtimeStorage_);

    LOG_INFO(TAG, "Hardware initialized successfully");
    return Result<void>();
}

Result<void> HardwareInitializer::initializeModbus() {
    // Initialize GPIO4 (TX) to ensure it starts LOW before UART takes control
    // This prevents the WS3081 transceiver from being stuck in transmit mode
    pinMode(RS485_TX_PIN, OUTPUT);
    digitalWrite(RS485_TX_PIN, LOW);
    LOG_INFO(TAG, "Set TX pin (GPIO%d) to LOW before UART init", RS485_TX_PIN);

    // Small delay to let the pin settle
    vTaskDelay(pdMS_TO_TICKS(10));

    // Initialize RS485 serial for Modbus (8E1: 8 data bits, Even parity, 1 stop bit)
    // All Modbus devices are configured for even parity
    Serial1.begin(MODBUS_BAUD_RATE, SERIAL_8E1, RS485_RX_PIN, RS485_TX_PIN);
    LOG_INFO(TAG, "Serial1 initialized at %d baud with RX:GPIO%d, TX:GPIO%d",
             MODBUS_BAUD_RATE, RS485_RX_PIN, RS485_TX_PIN);

    // Delay to ensure serial port and WS3081 transceiver are fully initialized
    vTaskDelay(pdMS_TO_TICKS(100));
    LOG_INFO(TAG, "Serial1 ready, initializing Modbus master...");

    // CRITICAL: Set the global ModbusRTU instance for ModbusDevice base class
    LOG_INFO(TAG, "Setting global ModbusRTU instance...");
    modbus::ModbusRegistry::getInstance().setModbusRTU(&SRP::getModbusMaster());

    // Small delay to ensure globalModbusRTU is properly set
    vTaskDelay(pdMS_TO_TICKS(50));

    // Register global handlers for routing responses to devices
    LOG_INFO(TAG, "Registering Modbus onData callback...");
    SRP::getModbusMaster().onData([](uint8_t serverAddress, esp32Modbus::FunctionCode fc,
                                     uint16_t address, const uint8_t* data, size_t length) {
        LOG_DEBUG(TAG, "Modbus data received - Addr: 0x%02X, FC: %d, StartAddr: 0x%04X, Len: %d",
                  serverAddress, fc, address, length);

        // Use the global ModbusDevice routing function
        mainHandleData(serverAddress, fc, address, data, length);
    });

    SRP::getModbusMaster().onError([](esp32Modbus::Error error) {
        LOG_ERROR(TAG, "Modbus communication error: %d", static_cast<int>(error));
    });

    LOG_INFO(TAG, "Modbus callbacks registered successfully");

    // Start the Modbus RTU task on core 1 to avoid interference with BLE on core 0
    SRP::getModbusMaster().begin(1);  // Pin to core 1
    LOG_INFO(TAG, "Modbus master initialized and started on core 1");

    // Brief delay for ModbusRTU task to initialize its queue
    vTaskDelay(pdMS_TO_TICKS(100));
    LOG_INFO(TAG, "Modbus RTU task should now be fully initialized");

    return Result<void>();
}

void HardwareInitializer::initializeTimezone() {
    // Set system timezone
    // CET-1CEST,M3.5.0,M10.5.0/3 means:
    // - CET (Central European Time) UTC+1
    // - CEST (Central European Summer Time) UTC+2
    // - DST starts last Sunday in March at 2:00
    // - DST ends last Sunday in October at 3:00
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    LOG_INFO(TAG, "System timezone set to CET/CEST");
}

bool HardwareInitializer::initializeRTC(DS3231Controller*& ds3231) {
    LOG_INFO(TAG, "Initializing DS3231 RTC...");
    SharedI2CInitializer::ensureI2CInitialized();

    // Suppress "Bus already started" warning from Adafruit library's redundant Wire.begin() call
    esp_log_level_set("Wire", ESP_LOG_ERROR);

    ds3231 = new DS3231Controller();
    if (!ds3231 || !ds3231->begin()) {
        LOG_WARN(TAG, "DS3231 not found - RTC features will be unavailable");
        delete ds3231;
        ds3231 = nullptr;
        return false;
    }

    // Verify DS3231 communication
    DateTime checkTime = ds3231->now();
    if (!checkTime.isValid() || checkTime.year() < 2020 || checkTime.year() > 2100) {
        LOG_WARN(TAG, "DS3231 returns invalid time - RTC features will be unavailable");
        delete ds3231;
        ds3231 = nullptr;
        return false;
    }

    LOG_INFO(TAG, "DS3231 initialized: %04d-%02d-%02d %02d:%02d:%02d",
             checkTime.year(), checkTime.month(), checkTime.day(),
             checkTime.hour(), checkTime.minute(), checkTime.second());

    // Set system time from RTC
    struct tm timeinfo;
    timeinfo.tm_year = checkTime.year() - 1900;
    timeinfo.tm_mon = checkTime.month() - 1;
    timeinfo.tm_mday = checkTime.day();
    timeinfo.tm_hour = checkTime.hour();
    timeinfo.tm_min = checkTime.minute();
    timeinfo.tm_sec = checkTime.second();
    timeinfo.tm_isdst = -1;  // Let system determine DST

    time_t rtcTime = mktime(&timeinfo);
    // Set system time from RTC
    // Note: DS3231 RTC only has 1-second resolution, so tv_usec will be 0
    struct timeval tv = { .tv_sec = rtcTime, .tv_usec = 0 };
    if (settimeofday(&tv, nullptr) == 0) {
        LOG_INFO(TAG, "System time set from RTC (sub-second precision: 0)");
    } else {
        LOG_WARN(TAG, "Failed to set system time from RTC");
    }

    return true;
}

bool HardwareInitializer::initializeFRAM(rtstorage::RuntimeStorage*& storage) {
    LOG_INFO(TAG, "Initializing RuntimeStorage (FRAM)...");
    storage = new rtstorage::RuntimeStorage();
    gRuntimeStorage = storage;

    if (!storage || !storage->begin(Wire, 0x50)) {
        LOG_WARN(TAG, "RuntimeStorage (FRAM) not found - runtime data will not persist");
        delete storage;
        storage = nullptr;
        gRuntimeStorage = nullptr;
        return false;
    }

    // Verify FRAM integrity
    if (!storage->verifyIntegrity()) {
        LOG_WARN(TAG, "FRAM data corrupted - formatting...");
        if (storage->format()) {
            LOG_INFO(TAG, "FRAM formatted successfully");
        } else {
            LOG_ERROR(TAG, "Failed to format FRAM");
            delete storage;
            storage = nullptr;
            gRuntimeStorage = nullptr;
            return false;
        }
    }

    uint32_t framSize = storage->getSize();
    LOG_INFO(TAG, "RuntimeStorage initialized: %lu bytes available", framSize);
    return true;
}
