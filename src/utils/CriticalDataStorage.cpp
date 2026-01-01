// src/utils/CriticalDataStorage.cpp
#include "CriticalDataStorage.h"
#include "config/SystemConstants.h"
#include <Wire.h>

// FRAM capacity constant (MB85RC256V = 256Kbit = 32KB)
static constexpr uint16_t FRAM_CAPACITY = 0x8000;  // 32768 bytes

// Static member definitions
rtstorage::RuntimeStorage* CriticalDataStorage::storage_ = nullptr;
CriticalDataStorage::EmergencyState CriticalDataStorage::cachedEmergency_ = {0};
CriticalDataStorage::PIDTuningData CriticalDataStorage::cachedPID_ = {0};
CriticalDataStorage::RuntimeCounters CriticalDataStorage::cachedCounters_ = {0};
uint16_t CriticalDataStorage::errorLogIndex_ = 0;
uint16_t CriticalDataStorage::safetyLogIndex_ = 0;
bool CriticalDataStorage::initialized_ = false;
SemaphoreHandle_t CriticalDataStorage::framMutex_ = nullptr;  // Round 20 Issue #2

// CRC32 implementation
uint32_t CriticalDataStorage::calculateCRC32(const void* data, size_t length) {
    const uint32_t polynomial = 0xEDB88320;
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t* bytes = (const uint8_t*)data;
    
    for (size_t i = 0; i < length; i++) {
        crc ^= bytes[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ polynomial;
            } else {
                crc >>= 1;
            }
        }
    }
    
    return ~crc;
}

// Low-level FRAM write
bool CriticalDataStorage::writeToFRAM(uint16_t address, const void* data, size_t size) {
    if (!storage_ || !storage_->isConnected()) {
        return false;
    }

    // Bounds check - ensure write won't exceed FRAM capacity
    if (static_cast<uint32_t>(address) + size > FRAM_CAPACITY) {
        LOG_ERROR("CriticalData", "FRAM write bounds error: addr=0x%04X size=%u exceeds 0x%04X",
                  address, size, FRAM_CAPACITY);
        return false;
    }

    // Round 20 Issue #2: Acquire mutex to protect I2C bus access
    if (!framMutex_ || xSemaphoreTake(framMutex_, pdMS_TO_TICKS(SystemConstants::Timing::MUTEX_FRAM_TIMEOUT_MS)) != pdTRUE) {
        LOG_ERROR("CriticalData", "Failed to acquire FRAM mutex for write");
        return false;
    }

    const uint8_t* bytes = (const uint8_t*)data;
    uint8_t i2cAddr = 0x50;  // Default FRAM address
    bool success = true;

    // Write in chunks (32 bytes max per write for safety)
    size_t written = 0;
    while (written < size) {
        size_t chunkSize = min((size_t)32, size - written);

        Wire.beginTransmission(i2cAddr);
        Wire.write((uint8_t)((address + written) >> 8));     // Address high byte
        Wire.write((uint8_t)((address + written) & 0xFF));   // Address low byte

        for (size_t i = 0; i < chunkSize; i++) {
            Wire.write(bytes[written + i]);
        }

        if (Wire.endTransmission() != 0) {
            LOG_ERROR("CriticalData", "FRAM write failed at address 0x%04X", address + written);
            success = false;
            break;
        }

        written += chunkSize;
        // Note: FRAM has no write cycle time (unlike EEPROM), writes are instantaneous
        // A small yield allows other tasks to run during large writes
        if (written < size) {
            taskYIELD();
        }
    }

    xSemaphoreGive(framMutex_);
    return success;
}

// Low-level FRAM read
bool CriticalDataStorage::readFromFRAM(uint16_t address, void* data, size_t size) {
    if (!storage_ || !storage_->isConnected()) {
        return false;
    }

    // Bounds check - ensure read won't exceed FRAM capacity
    if (static_cast<uint32_t>(address) + size > FRAM_CAPACITY) {
        LOG_ERROR("CriticalData", "FRAM read bounds error: addr=0x%04X size=%u exceeds 0x%04X",
                  address, size, FRAM_CAPACITY);
        return false;
    }

    // Round 20 Issue #2: Acquire mutex to protect I2C bus access
    if (!framMutex_ || xSemaphoreTake(framMutex_, pdMS_TO_TICKS(SystemConstants::Timing::MUTEX_FRAM_TIMEOUT_MS)) != pdTRUE) {
        LOG_ERROR("CriticalData", "Failed to acquire FRAM mutex for read");
        return false;
    }

    uint8_t* bytes = (uint8_t*)data;
    uint8_t i2cAddr = 0x50;  // Default FRAM address
    bool success = true;

    // Set address to read from
    Wire.beginTransmission(i2cAddr);
    Wire.write((uint8_t)(address >> 8));     // Address high byte
    Wire.write((uint8_t)(address & 0xFF));   // Address low byte

    if (Wire.endTransmission() != 0) {
        LOG_ERROR("CriticalData", "FRAM read setup failed at address 0x%04X", address);
        xSemaphoreGive(framMutex_);
        return false;
    }

    // Read data in chunks
    size_t bytesRead = 0;
    while (bytesRead < size) {
        size_t chunkSize = min((size_t)32, size - bytesRead);

        Wire.requestFrom(i2cAddr, (uint8_t)chunkSize);

        uint32_t timeout = millis() + SystemConstants::Communication::I2C_READ_TIMEOUT_MS;
        while (Wire.available() < (int)chunkSize && millis() < timeout) {
            taskYIELD();  // FreeRTOS-friendly yield instead of blocking delay
        }

        if (Wire.available() < (int)chunkSize) {
            LOG_ERROR("CriticalData", "FRAM read timeout at address 0x%04X", address + bytesRead);
            success = false;
            break;
        }

        for (size_t i = 0; i < chunkSize; i++) {
            bytes[bytesRead++] = Wire.read();
        }
    }

    xSemaphoreGive(framMutex_);
    return success;
}