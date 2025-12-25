// src/utils/LibraryErrorMapper.h
#ifndef LIBRARY_ERROR_MAPPER_H
#define LIBRARY_ERROR_MAPPER_H

#include "ErrorHandler.h"
#include <EthernetManager.h>
#include <MB8ART.h>
#include <RYN4.h>
#include <MQTTManager.h>
#include <IDeviceInstance.h>

/**
 * @brief Maps library-specific error codes to SystemError
 * 
 * This class provides mapping functions for converting error codes from
 * various libraries (EthernetManager, MB8ART, RYN4, MQTTManager, etc.)
 * to the unified SystemError enum.
 */
class LibraryErrorMapper {
public:
    /**
     * @brief Map EthernetManager error codes to SystemError
     */
    static SystemError mapEthernetError(EthError ethError) {
        switch (ethError) {
            case EthError::OK:
                return SystemError::SUCCESS;
            case EthError::INVALID_PARAMETER:
                return SystemError::CONFIG_INVALID;
            case EthError::MUTEX_TIMEOUT:
                return SystemError::MUTEX_TIMEOUT;
            case EthError::ALREADY_INITIALIZED:
                return SystemError::ALREADY_INITIALIZED;
            case EthError::NOT_INITIALIZED:
                return SystemError::NOT_INITIALIZED;
            case EthError::PHY_START_FAILED:
                return SystemError::ETHERNET_PHY_ERROR;
            case EthError::CONFIG_FAILED:
                return SystemError::CONFIG_INVALID;
            case EthError::CONNECTION_TIMEOUT:
                return SystemError::NETWORK_TIMEOUT;
            case EthError::EVENT_HANDLER_FAILED:
                return SystemError::NETWORK_INIT_FAILED;
            case EthError::MEMORY_ALLOCATION_FAILED:
                return SystemError::MEMORY_ALLOCATION_FAILED;
            case EthError::NETIF_ERROR:
                return SystemError::NETWORK_INIT_FAILED;
            case EthError::UNKNOWN_ERROR:
            default:
                return SystemError::UNKNOWN_ERROR;
        }
    }
    
    /**
     * @brief Map generic device errors to SystemError
     */
    static SystemError mapDeviceError(IDeviceInstance::DeviceError deviceError) {
        switch (deviceError) {
            case IDeviceInstance::DeviceError::SUCCESS:
                return SystemError::SUCCESS;
            case IDeviceInstance::DeviceError::NOT_INITIALIZED:
                return SystemError::DEVICE_NOT_INITIALIZED;
            case IDeviceInstance::DeviceError::TIMEOUT:
                return SystemError::TIMEOUT;
            case IDeviceInstance::DeviceError::MUTEX_ERROR:
                return SystemError::MUTEX_TIMEOUT;
            case IDeviceInstance::DeviceError::COMMUNICATION_ERROR:
                return SystemError::MODBUS_COMMUNICATION_ERROR;
            case IDeviceInstance::DeviceError::INVALID_PARAMETER:
                return SystemError::INVALID_PARAMETER;
            case IDeviceInstance::DeviceError::UNKNOWN_ERROR:
            default:
                return SystemError::UNKNOWN_ERROR;
        }
    }
    
    /**
     * @brief Map MB8ART sensor error codes to SystemError
     * Note: mb8art::SensorErrorCode no longer exists in new library
     */
    static SystemError mapMB8ARTError(int errorCode) {
        // New library doesn't expose error codes, return generic error
        return SystemError::UNKNOWN_ERROR;
    }
    
    /**
     * @brief Map RYN4 relay error codes to SystemError
     */
    static SystemError mapRYN4Error(ryn4::RelayErrorCode relayError) {
        using namespace ryn4;
        switch (relayError) {
            case RelayErrorCode::SUCCESS:
                return SystemError::SUCCESS;
            case RelayErrorCode::INVALID_INDEX:
                return SystemError::CONFIG_INVALID;
            case RelayErrorCode::MODBUS_ERROR:
                return SystemError::MODBUS_INVALID_RESPONSE;
            case RelayErrorCode::TIMEOUT:
                return SystemError::MODBUS_TIMEOUT;
            case RelayErrorCode::MUTEX_ERROR:
                return SystemError::MUTEX_TIMEOUT;
            case RelayErrorCode::NOT_INITIALIZED:
                return SystemError::NOT_INITIALIZED;
            case RelayErrorCode::UNKNOWN_ERROR:
            default:
                return SystemError::UNKNOWN_ERROR;
        }
    }
    
    /**
     * @brief Map MQTTManager error codes to SystemError
     */
    static SystemError mapMQTTError(MQTTError mqttError) {
        switch (mqttError) {
            case MQTTError::OK:
                return SystemError::SUCCESS;
            case MQTTError::NOT_INITIALIZED:
                return SystemError::NOT_INITIALIZED;
            case MQTTError::ALREADY_CONNECTED:
                return SystemError::ALREADY_INITIALIZED;
            case MQTTError::CONNECTION_FAILED:
                return SystemError::MQTT_CONNECT_FAILED;
            case MQTTError::BROKER_UNREACHABLE:
                return SystemError::MQTT_BROKER_UNREACHABLE;
            case MQTTError::PUBLISH_FAILED:
                return SystemError::MQTT_PUBLISH_FAILED;
            case MQTTError::SUBSCRIBE_FAILED:
                return SystemError::MQTT_SUBSCRIBE_FAILED;
            case MQTTError::INVALID_PARAMETER:
                return SystemError::INVALID_PARAMETER;
            case MQTTError::MEMORY_ALLOCATION_FAILED:
                return SystemError::MEMORY_ALLOCATION_FAILED;
            case MQTTError::TIMEOUT:
                return SystemError::NETWORK_TIMEOUT;
            case MQTTError::UNKNOWN_ERROR:
            default:
                return SystemError::UNKNOWN_ERROR;
        }
    }
    
    /**
     * @brief Map OTAManager error codes to SystemError
     * 
     * NOTE: Using generic integer error codes as OTAManager doesn't
     * define a specific error enum. This mapping covers common OTA
     * error scenarios based on ESP32 OTA library conventions.
     */
    static SystemError mapOTAError(int otaError) {
        // Mapping based on common ESP32 OTA error patterns
        switch (otaError) {
            case 0:  // Success
                return SystemError::SUCCESS;
            case -1: // Network not connected
                return SystemError::NETWORK_NOT_CONNECTED;
            case -2: // Invalid firmware
                return SystemError::CONFIG_INVALID;
            case -3: // Memory allocation failed
                return SystemError::MEMORY_ALLOCATION_FAILED;
            default:
                return SystemError::UNKNOWN_ERROR;
        }
    }
    
    /**
     * @brief Convert EthernetManager Result to system Result
     */
    static Result<void> convertEthResult(const EthResult<void>& ethResult) {
        if (ethResult.isOk()) {
            return Result<void>();
        }
        SystemError sysError = mapEthernetError(ethResult.error());
        return Result<void>(sysError, ErrorHandler::errorToString(sysError));
    }

    /**
     * @brief Convert EthernetManager Result<T> to system Result<T>
     */
    template<typename T>
    static Result<T> convertEthResult(const EthResult<T>& ethResult) {
        if (ethResult.isOk()) {
            return Result<T>(ethResult.value());
        }
        SystemError sysError = mapEthernetError(ethResult.error());
        return Result<T>(sysError, ErrorHandler::errorToString(sysError));
    }
    
    /**
     * @brief Convert MB8ART SensorResult to system Result
     * Note: mb8art::SensorResult no longer exists in new library
     */
    static Result<void> convertMB8ARTResult(int errorCode) {
        if (errorCode == 0) {
            return Result<void>();
        }
        SystemError sysError = mapMB8ARTError(errorCode);
        return Result<void>(sysError, ErrorHandler::errorToString(sysError));
    }
    
    /**
     * @brief Convert RYN4 RelayResult to system Result
     */
    static Result<void> convertRYN4Result(const ryn4::RelayResult<void>& relayResult) {
        if (relayResult.isOk()) {
            return Result<void>();
        }
        SystemError sysError = mapRYN4Error(relayResult.error());
        return Result<void>(sysError, ErrorHandler::errorToString(sysError));
    }

    /**
     * @brief Convert MQTTManager MQTTResult to system Result
     */
    static Result<void> convertMQTTResult(const MQTTResult<void>& mqttResult) {
        if (mqttResult.isOk()) {
            return Result<void>();
        }
        SystemError sysError = mapMQTTError(mqttResult.error());
        return Result<void>(sysError, ErrorHandler::errorToString(sysError));
    }
};

#endif // LIBRARY_ERROR_MAPPER_H