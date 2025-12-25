// src/init/TaskInitializer.h
#pragma once

#include "utils/ErrorHandler.h"

// Forward declarations
class SystemInitializer;

/**
 * @brief Handles initialization of FreeRTOS tasks
 *
 * This class is a friend of SystemInitializer and handles the creation
 * of all system tasks including sensor, control, and monitoring tasks.
 */
class TaskInitializer {
public:
    /**
     * @brief Initialize all system tasks
     * @param initializer Pointer to the SystemInitializer instance
     * @return Result indicating success or failure
     */
    static Result<void> initializeTasks(SystemInitializer* initializer);

    /**
     * @brief Create MB8ART sensor tasks (processing and data acquisition)
     * @param initializer Pointer to the SystemInitializer instance
     */
    static void createMB8ARTTasks(SystemInitializer* initializer);

    /**
     * @brief Create heating control task
     * @param initializer Pointer to the SystemInitializer instance
     */
    static void createHeatingControlTask(SystemInitializer* initializer);

    /**
     * @brief Create water control task
     * @param initializer Pointer to the SystemInitializer instance
     */
    static void createWaterControlTask(SystemInitializer* initializer);

    /**
     * @brief Create burner control task
     * @param initializer Pointer to the SystemInitializer instance
     */
    static void createBurnerControlTask(SystemInitializer* initializer);

    /**
     * @brief Initialize burner control task with full setup
     * @param initializer Pointer to the SystemInitializer instance
     * @return Result indicating success or failure
     */
    static Result<void> initializeBurnerControlTask(SystemInitializer* initializer);

private:
    /**
     * @brief Initialize relay control task
     * @param initializer Pointer to the SystemInitializer instance
     */
    static void initializeRelayControlTask(SystemInitializer* initializer);

    /**
     * @brief Initialize OTA task
     */
    static void initializeOTATask();

    /**
     * @brief Initialize sensor tasks (ANDRTF3, etc.)
     * @param initializer Pointer to the SystemInitializer instance
     */
    static void initializeSensorTasks(SystemInitializer* initializer);

    /**
     * @brief Initialize control tasks (heating, water, burner, PID)
     * @param initializer Pointer to the SystemInitializer instance
     */
    static void initializeControlTasks(SystemInitializer* initializer);

    /**
     * @brief Initialize pump control tasks
     * @param initializer Pointer to the SystemInitializer instance
     */
    static void initializePumpTasks(SystemInitializer* initializer);

    /**
     * @brief Initialize monitoring task
     */
    static void initializeMonitoringTask();

    /**
     * @brief Initialize boiler temperature control task (cascade control inner loop)
     * @param initializer Pointer to the SystemInitializer instance
     */
    static void initializeBoilerTempControlTask(SystemInitializer* initializer);
};
