#ifndef TASK_DEPENDENCY_MANAGER_H
#define TASK_DEPENDENCY_MANAGER_H

#include <unordered_map>
#include <vector>
#include <string>
#include <functional>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

/**
 * @brief Manages task dependencies and startup order
 * 
 * Ensures tasks start in correct order, verifies dependencies are met,
 * and provides health monitoring with automatic recovery.
 */
class TaskDependencyManager {
public:
    // Task states
    enum class TaskState {
        NOT_STARTED,
        STARTING,
        RUNNING,
        FAILED,
        STOPPED,
        RESTARTING
    };

    // Task information
    struct TaskInfo {
        std::string name;
        TaskFunction_t function;
        uint32_t stackSize;
        UBaseType_t priority;
        void* parameters;
        TaskHandle_t handle;
        TaskState state;
        std::vector<std::string> dependencies;
        uint32_t lastHealthCheck;
        uint8_t restartCount;
        bool critical;  // System cannot run without this task
    };

    // Health check function
    using HealthCheckFunc = std::function<bool(const TaskInfo&)>;

    /**
     * @brief Get singleton instance
     */
    static TaskDependencyManager& getInstance();

    /**
     * @brief Register a task with its dependencies
     * @param name Task name
     * @param function Task function
     * @param stackSize Stack size
     * @param priority Task priority
     * @param dependencies List of task names this depends on
     * @param critical Whether task is critical for system operation
     */
    void registerTask(
        const std::string& name,
        TaskFunction_t function,
        uint32_t stackSize,
        UBaseType_t priority,
        const std::vector<std::string>& dependencies = {},
        bool critical = false
    );

    /**
     * @brief Set health check function for task
     * @param taskName Task name
     * @param healthCheck Function to check task health
     */
    void setHealthCheck(const std::string& taskName, HealthCheckFunc healthCheck);

    /**
     * @brief Start all tasks in dependency order
     * @return true if all critical tasks started successfully
     */
    bool startAllTasks();

    /**
     * @brief Start specific task (checks dependencies first)
     * @param taskName Task to start
     * @return true if task started successfully
     */
    bool startTask(const std::string& taskName);

    /**
     * @brief Stop task and dependent tasks
     * @param taskName Task to stop
     */
    void stopTask(const std::string& taskName);

    /**
     * @brief Restart task
     * @param taskName Task to restart
     * @return true if restart successful
     */
    bool restartTask(const std::string& taskName);

    /**
     * @brief Check if task is running
     * @param taskName Task name
     * @return true if task is in RUNNING state
     */
    bool isTaskRunning(const std::string& taskName) const;

    /**
     * @brief Check all task dependencies
     * @param taskName Task name
     * @return true if all dependencies are running
     */
    bool areDependenciesMet(const std::string& taskName) const;

    /**
     * @brief Get task state
     * @param taskName Task name
     * @return Current task state
     */
    TaskState getTaskState(const std::string& taskName) const;

    /**
     * @brief Perform health check on all tasks
     * @return Number of unhealthy tasks
     */
    uint8_t performHealthCheck();

    /**
     * @brief Get list of failed tasks
     */
    std::vector<std::string> getFailedTasks() const;

    /**
     * @brief Get list of tasks dependent on given task
     */
    std::vector<std::string> getDependentTasks(const std::string& taskName) const;

    /**
     * @brief Enable/disable automatic restart
     */
    void setAutoRestartEnabled(bool enabled) { autoRestartEnabled = enabled; }

    /**
     * @brief Set maximum restart attempts
     */
    void setMaxRestartAttempts(uint8_t attempts) { maxRestartAttempts = attempts; }

    /**
     * @brief Wait for task to reach specific state
     * @param taskName Task name
     * @param state Target state
     * @param timeoutMs Timeout in milliseconds
     * @return true if state reached within timeout
     */
    bool waitForTaskState(
        const std::string& taskName,
        TaskState state,
        uint32_t timeoutMs
    );

private:
    TaskDependencyManager();
    ~TaskDependencyManager() = default;
    TaskDependencyManager(const TaskDependencyManager&) = delete;
    TaskDependencyManager& operator=(const TaskDependencyManager&) = delete;

    // Task registry
    std::unordered_map<std::string, TaskInfo> tasks;
    
    // Health check functions
    std::unordered_map<std::string, HealthCheckFunc> healthChecks;
    
    // Dependency graph (reverse - who depends on me)
    std::unordered_map<std::string, std::vector<std::string>> dependents;
    
    // Configuration
    bool autoRestartEnabled;
    uint8_t maxRestartAttempts;
    uint32_t healthCheckIntervalMs;
    uint32_t restartDelayMs;
    
    // Synchronization
    SemaphoreHandle_t mutex;
    EventGroupHandle_t stateEventGroup;
    
    // Health monitor task
    TaskHandle_t healthMonitorTask;
    
    /**
     * @brief Build dependency graph
     */
    void buildDependencyGraph();
    
    /**
     * @brief Topological sort for startup order
     */
    std::vector<std::string> getStartupOrder();
    
    /**
     * @brief Start task internal implementation
     */
    bool startTaskInternal(TaskInfo& task);
    
    /**
     * @brief Stop task internal implementation
     */
    void stopTaskInternal(TaskInfo& task);
    
    /**
     * @brief Default health check
     */
    static bool defaultHealthCheck(const TaskInfo& task);
    
    /**
     * @brief Health monitoring task
     */
    static void healthMonitorTaskFunction(void* pvParameters);
    
    /**
     * @brief Handle task failure
     */
    void handleTaskFailure(const std::string& taskName);
    
    /**
     * @brief Update task state
     */
    void updateTaskState(const std::string& taskName, TaskState newState);
    
    /**
     * @brief Notify state change
     */
    void notifyStateChange(const std::string& taskName, TaskState newState);
};

// Standard task dependencies for the system
namespace TaskDependencies {
    // Core infrastructure
    const std::vector<std::string> NETWORK_DEPS = {};
    const std::vector<std::string> MQTT_DEPS = {"NetworkTask"};
    
    // Sensor tasks
    const std::vector<std::string> MB8ART_DEPS = {};
    const std::vector<std::string> RYN4_DEPS = {};
    const std::vector<std::string> BLE_SENSOR_DEPS = {};
    
    // Control tasks
    const std::vector<std::string> BURNER_CONTROL_DEPS = {"MB8ARTTask", "RYN4Task"};
    const std::vector<std::string> HEATING_CONTROL_DEPS = {"MB8ARTTask", "BurnerControlTask"};
    const std::vector<std::string> WATER_CONTROL_DEPS = {"MB8ARTTask", "BurnerControlTask"};
    
    // Monitoring tasks
    const std::vector<std::string> MONITORING_DEPS = {"MQTTTask"};
    const std::vector<std::string> DIAGNOSTICS_DEPS = {"MQTTTask"};
}

#endif // TASK_DEPENDENCY_MANAGER_H