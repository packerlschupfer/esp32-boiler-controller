// src/init/SystemInitializer.h
#ifndef SYSTEM_INITIALIZER_H
#define SYSTEM_INITIALIZER_H

#include <memory>
#include <vector>
#include "utils/ErrorHandler.h"
#include "config/ProjectConfig.h"

// Forward declarations
class MB8ART;
class RYN4;
namespace andrtf3 { class ANDRTF3; }
class DS3231Controller;
namespace rtstorage { class RuntimeStorage; }
class MQTTManager;
class HeatingControlModule;
class WheaterControlModule;
class PIDControlModule;

// Friend class forward declarations
class ModbusDeviceInitializer;
class TaskInitializer;
class HardwareInitializer;

/**
 * @brief Manages system initialization with proper error handling and cleanup
 * 
 * This class ensures that all components are initialized in the correct order
 * and provides automatic cleanup if initialization fails at any stage.
 */
class SystemInitializer {
    // Friend classes for modular initialization
    friend class ModbusDeviceInitializer;
    friend class TaskInitializer;
    friend class HardwareInitializer;

public:
    enum class InitStage {
        NONE = 0,
        LOGGING,
        SHARED_RESOURCES,
        HARDWARE,
        NETWORK,
        MODBUS_DEVICES,
        CONTROL_MODULES,
        MQTT,
        TASKS,
        COMPLETE
    };
    
    /**
     * @brief Constructor
     */
    SystemInitializer();
    
    /**
     * @brief Destructor - performs cleanup if needed
     */
    ~SystemInitializer();
    
    /**
     * @brief Initialize the entire system
     * @return Result indicating success or failure
     */
    Result<void> initializeSystem();
    
    /**
     * @brief Get current initialization stage
     */
    InitStage getCurrentStage() const { return currentStage_; }
    
    /**
     * @brief Check if system is fully initialized
     */
    bool isFullyInitialized() const { return currentStage_ == InitStage::COMPLETE; }
    
    /**
     * @brief Force cleanup of all initialized resources
     */
    void cleanup();
    
    // Getters for initialized components (direct member access - ServiceContainer removed)
    MB8ART* getMB8ART() const;
    RYN4* getRYN4() const;
    andrtf3::ANDRTF3* getANDRTF3() const;
    MQTTManager* getMQTTManager() const;
    PIDControlModule* getPIDControl() const;
    HeatingControlModule* getHeatingControl() const;
    WheaterControlModule* getWheaterControl() const;
    class BurnerSystemController* getBurnerSystemController() const;
    DS3231Controller* getDS3231() const;
    
    // Post-initialization methods
    Result<void> initializeBurnerControlTask();
    
private:
    // Initialization stages
    Result<void> initializeLogging();
    Result<void> initializeSharedResources();
    Result<void> initializeHardware();
    Result<void> initializeNetwork();
    Result<void> initializeNetworkAsync();  // Non-blocking network init
    Result<void> initializeModbusDevices();
    Result<void> initializeControlModules();
    Result<void> initializeMQTT();
    Result<void> initializeTasks();
    
    // Cleanup stages (reverse order)
    void cleanupTasks();
    void cleanupMQTT();
    void cleanupControlModules();
    void cleanupModbusDevices();
    void cleanupNetwork();
    void cleanupHardware();
    void cleanupSharedResources();
    void cleanupLogging();
    
    // Track initialization progress
    InitStage currentStage_;
    
    // Device pointers
    MB8ART* mb8art_;
    RYN4* ryn4_;
    andrtf3::ANDRTF3* andrtf3_;
    DS3231Controller* ds3231_;
    rtstorage::RuntimeStorage* runtimeStorage_;
    
    // Manager pointers
    MQTTManager* mqttManager_;
    
    // Control module pointers
    HeatingControlModule* heatingControl_;
    WheaterControlModule* wheaterControl_;
    PIDControlModule* pidControl_;

    // Unified burner system controller (controls only burner relays)
    class BurnerSystemController* burnerSystemController_;
    
    // Mutex handles that need cleanup
    struct MutexInfo {
        SemaphoreHandle_t* handle;
        const char* name;
    };
    std::vector<MutexInfo> createdMutexes_;
    
    // Event group handles that need cleanup
    struct EventGroupInfo {
        EventGroupHandle_t* handle;
        const char* name;
    };
    std::vector<EventGroupInfo> createdEventGroups_;
    
    // Task handles that need cleanup
    struct TaskInfo {
        TaskHandle_t handle;
        const char* name;
    };
    std::vector<TaskInfo> createdTasks_;
    
    // Helper methods
    bool createMutex(SemaphoreHandle_t* mutex, const char* name);
    void createMB8ARTTasks();
    void createHeatingControlTask();
    void createWaterControlTask();
    void createBurnerControlTask();
    bool createEventGroup(EventGroupHandle_t* group, const char* name);
    void registerTask(TaskHandle_t handle, const char* name);
    
    // Device ready event group for synchronization
    EventGroupHandle_t deviceReadyEventGroup_;
};

// Note: SystemInitializer is accessed via gSystemInitializer global pointer

#endif // SYSTEM_INITIALIZER_H