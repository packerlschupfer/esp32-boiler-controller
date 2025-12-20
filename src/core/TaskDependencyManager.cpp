// src/core/TaskDependencyManager.cpp
#include "core/TaskDependencyManager.h"
#include "core/SystemResourceProvider.h"
#include "config/SystemConstants.h"
#include "utils/Utils.h"
#include "LoggingMacros.h"
#include <algorithm>
#include <queue>
#include <set>

static const char* TAG = "TaskDependencyManager";

// Bounded mutex timeout - NEVER use MUTEX_TIMEOUT to prevent deadlock
static constexpr TickType_t MUTEX_TIMEOUT = pdMS_TO_TICKS(SystemConstants::Timing::MUTEX_LONG_TIMEOUT_MS);

TaskDependencyManager::TaskDependencyManager() 
    : autoRestartEnabled(true)
    , maxRestartAttempts(3)
    , healthCheckIntervalMs(30000)  // 30 seconds
    , restartDelayMs(5000) {
    
    // Create synchronization primitives
    mutex = xSemaphoreCreateMutex();
    if (!mutex) {
        LOG_ERROR(TAG, "Failed to create mutex");
    }
    
    stateEventGroup = xEventGroupCreate();
    if (!stateEventGroup) {
        LOG_ERROR(TAG, "Failed to create event group");
    }
}

TaskDependencyManager& TaskDependencyManager::getInstance() {
    static TaskDependencyManager instance;
    return instance;
}

void TaskDependencyManager::registerTask(
    const std::string& name,
    TaskFunction_t function,
    uint32_t stackSize,
    UBaseType_t priority,
    const std::vector<std::string>& dependencies,
    bool critical) {
    
    if (xSemaphoreTake(mutex, MUTEX_TIMEOUT) == pdTRUE) {
        TaskInfo info = {
            .name = name,
            .function = function,
            .stackSize = stackSize,
            .priority = priority,
            .parameters = nullptr,
            .handle = nullptr,
            .state = TaskState::NOT_STARTED,
            .dependencies = dependencies,
            .lastHealthCheck = 0,
            .restartCount = 0,
            .critical = critical
        };
        
        tasks[name] = info;
        
        // Build reverse dependency graph
        buildDependencyGraph();
        
        xSemaphoreGive(mutex);
        LOG_INFO(TAG, "Registered task: %s (critical: %d, deps: %d)", 
                 name.c_str(), critical, dependencies.size());
    }
}

void TaskDependencyManager::setHealthCheck(const std::string& taskName, HealthCheckFunc healthCheck) {
    if (xSemaphoreTake(mutex, MUTEX_TIMEOUT) == pdTRUE) {
        healthChecks[taskName] = healthCheck;
        xSemaphoreGive(mutex);
    }
}

bool TaskDependencyManager::startAllTasks() {
    LOG_INFO(TAG, "Starting all tasks in dependency order");
    
    // Get startup order
    std::vector<std::string> startupOrder = getStartupOrder();
    
    if (startupOrder.empty()) {
        LOG_ERROR(TAG, "Failed to determine startup order - possible circular dependency");
        return false;
    }
    
    bool allCriticalStarted = true;
    
    // Start tasks in order
    for (const auto& taskName : startupOrder) {
        if (!startTask(taskName)) {
            if (xSemaphoreTake(mutex, MUTEX_TIMEOUT) == pdTRUE) {
                if (tasks[taskName].critical) {
                    LOG_ERROR(TAG, "Failed to start critical task: %s", taskName.c_str());
                    allCriticalStarted = false;
                } else {
                    LOG_WARN(TAG, "Failed to start non-critical task: %s", taskName.c_str());
                }
                xSemaphoreGive(mutex);
            }
        }
    }
    
    // Start health monitoring if enabled
    if (autoRestartEnabled && !healthMonitorTask) {
        xTaskCreate(
            healthMonitorTaskFunction,
            "TaskHealthMonitor",
            2048,
            this,
            tskIDLE_PRIORITY + 1,
            &healthMonitorTask
        );
    }
    
    return allCriticalStarted;
}

bool TaskDependencyManager::startTask(const std::string& taskName) {
    if (xSemaphoreTake(mutex, MUTEX_TIMEOUT) == pdTRUE) {
        auto it = tasks.find(taskName);
        if (it == tasks.end()) {
            xSemaphoreGive(mutex);
            LOG_ERROR(TAG, "Task not found: %s", taskName.c_str());
            return false;
        }
        
        TaskInfo& task = it->second;
        
        // Check if already running
        if (task.state == TaskState::RUNNING) {
            xSemaphoreGive(mutex);
            LOG_WARN(TAG, "Task already running: %s", taskName.c_str());
            return true;
        }
        
        // Check dependencies
        if (!areDependenciesMet(taskName)) {
            xSemaphoreGive(mutex);
            LOG_ERROR(TAG, "Dependencies not met for task: %s", taskName.c_str());
            return false;
        }
        
        xSemaphoreGive(mutex);
        
        // Start the task
        return startTaskInternal(task);
    }
    
    return false;
}

void TaskDependencyManager::stopTask(const std::string& taskName) {
    if (xSemaphoreTake(mutex, MUTEX_TIMEOUT) == pdTRUE) {
        auto it = tasks.find(taskName);
        if (it != tasks.end()) {
            TaskInfo& task = it->second;
            
            // Get dependent tasks before stopping
            std::vector<std::string> deps = getDependentTasks(taskName);
            
            xSemaphoreGive(mutex);
            
            // Stop dependent tasks first
            for (const auto& dep : deps) {
                LOG_INFO(TAG, "Stopping dependent task: %s", dep.c_str());
                stopTask(dep);
            }
            
            // Stop the task itself
            stopTaskInternal(task);
        } else {
            xSemaphoreGive(mutex);
        }
    }
}

bool TaskDependencyManager::restartTask(const std::string& taskName) {
    LOG_INFO(TAG, "Restarting task: %s", taskName.c_str());
    
    // Stop the task
    stopTask(taskName);
    
    // Wait before restart
    vTaskDelay(pdMS_TO_TICKS(restartDelayMs));
    
    // Start the task
    return startTask(taskName);
}

bool TaskDependencyManager::isTaskRunning(const std::string& taskName) const {
    bool running = false;
    if (xSemaphoreTake(mutex, MUTEX_TIMEOUT) == pdTRUE) {
        auto it = tasks.find(taskName);
        if (it != tasks.end()) {
            running = (it->second.state == TaskState::RUNNING);
        }
        xSemaphoreGive(mutex);
    }
    return running;
}

bool TaskDependencyManager::areDependenciesMet(const std::string& taskName) const {
    auto it = tasks.find(taskName);
    if (it == tasks.end()) {
        return false;
    }
    
    const TaskInfo& task = it->second;
    
    // Check each dependency
    for (const auto& dep : task.dependencies) {
        auto depIt = tasks.find(dep);
        if (depIt == tasks.end() || depIt->second.state != TaskState::RUNNING) {
            LOG_WARN(TAG, "Dependency %s not running for task %s", dep.c_str(), taskName.c_str());
            return false;
        }
    }
    
    return true;
}

TaskDependencyManager::TaskState TaskDependencyManager::getTaskState(const std::string& taskName) const {
    TaskState state = TaskState::NOT_STARTED;
    if (xSemaphoreTake(mutex, MUTEX_TIMEOUT) == pdTRUE) {
        auto it = tasks.find(taskName);
        if (it != tasks.end()) {
            state = it->second.state;
        }
        xSemaphoreGive(mutex);
    }
    return state;
}

uint8_t TaskDependencyManager::performHealthCheck() {
    uint8_t unhealthyCount = 0;
    std::vector<std::string> tasksToCheck;
    
    // Get list of running tasks
    if (xSemaphoreTake(mutex, MUTEX_TIMEOUT) == pdTRUE) {
        for (const auto& [name, task] : tasks) {
            if (task.state == TaskState::RUNNING) {
                tasksToCheck.push_back(name);
            }
        }
        xSemaphoreGive(mutex);
    }
    
    // Check each task
    for (const auto& taskName : tasksToCheck) {
        bool healthy = false;
        
        // Use custom health check if available
        auto it = healthChecks.find(taskName);
        if (it != healthChecks.end()) {
            if (xSemaphoreTake(mutex, MUTEX_TIMEOUT) == pdTRUE) {
                healthy = it->second(tasks[taskName]);
                xSemaphoreGive(mutex);
            }
        } else {
            // Use default health check
            if (xSemaphoreTake(mutex, MUTEX_TIMEOUT) == pdTRUE) {
                healthy = defaultHealthCheck(tasks[taskName]);
                xSemaphoreGive(mutex);
            }
        }
        
        if (!healthy) {
            unhealthyCount++;
            handleTaskFailure(taskName);
        } else {
            // Update last health check time
            if (xSemaphoreTake(mutex, MUTEX_TIMEOUT) == pdTRUE) {
                tasks[taskName].lastHealthCheck = millis();
                xSemaphoreGive(mutex);
            }
        }
    }
    
    return unhealthyCount;
}

std::vector<std::string> TaskDependencyManager::getFailedTasks() const {
    std::vector<std::string> failed;
    if (xSemaphoreTake(mutex, MUTEX_TIMEOUT) == pdTRUE) {
        for (const auto& [name, task] : tasks) {
            if (task.state == TaskState::FAILED) {
                failed.push_back(name);
            }
        }
        xSemaphoreGive(mutex);
    }
    return failed;
}

std::vector<std::string> TaskDependencyManager::getDependentTasks(const std::string& taskName) const {
    std::vector<std::string> deps;
    auto it = dependents.find(taskName);
    if (it != dependents.end()) {
        deps = it->second;
    }
    return deps;
}

bool TaskDependencyManager::waitForTaskState(
    const std::string& taskName,
    TaskState state,
    uint32_t timeoutMs) {
    
    uint32_t startTime = millis();

    while (Utils::elapsedMs(startTime) < timeoutMs) {
        if (getTaskState(taskName) == state) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    return false;
}

void TaskDependencyManager::buildDependencyGraph() {
    dependents.clear();
    
    // Build reverse dependency mapping
    for (const auto& [taskName, task] : tasks) {
        for (const auto& dep : task.dependencies) {
            dependents[dep].push_back(taskName);
        }
    }
}

std::vector<std::string> TaskDependencyManager::getStartupOrder() {
    std::vector<std::string> order;
    std::set<std::string> visited;
    std::set<std::string> recursionStack;
    
    // Topological sort using DFS
    std::function<bool(const std::string&)> visit = [&](const std::string& taskName) -> bool {
        if (recursionStack.find(taskName) != recursionStack.end()) {
            LOG_ERROR(TAG, "Circular dependency detected at task: %s", taskName.c_str());
            return false;
        }
        
        if (visited.find(taskName) != visited.end()) {
            return true;
        }
        
        visited.insert(taskName);
        recursionStack.insert(taskName);
        
        auto it = tasks.find(taskName);
        if (it != tasks.end()) {
            // Visit dependencies first
            for (const auto& dep : it->second.dependencies) {
                if (!visit(dep)) {
                    return false;
                }
            }
        }
        
        recursionStack.erase(taskName);
        order.push_back(taskName);
        return true;
    };
    
    // Visit all tasks
    for (const auto& [taskName, task] : tasks) {
        if (!visit(taskName)) {
            return {}; // Circular dependency detected
        }
    }
    
    return order;
}

bool TaskDependencyManager::startTaskInternal(TaskInfo& task) {
    updateTaskState(task.name, TaskState::STARTING);
    
    BaseType_t result = xTaskCreate(
        task.function,
        task.name.c_str(),
        task.stackSize,
        task.parameters,
        task.priority,
        &task.handle
    );
    
    if (result == pdPASS) {
        updateTaskState(task.name, TaskState::RUNNING);
        LOG_INFO(TAG, "Started task: %s", task.name.c_str());
        return true;
    } else {
        updateTaskState(task.name, TaskState::FAILED);
        LOG_ERROR(TAG, "Failed to start task: %s", task.name.c_str());
        return false;
    }
}

void TaskDependencyManager::stopTaskInternal(TaskInfo& task) {
    if (task.handle != nullptr && task.state == TaskState::RUNNING) {
        LOG_INFO(TAG, "Stopping task: %s", task.name.c_str());
        
        updateTaskState(task.name, TaskState::STOPPED);
        
        // Delete the task
        vTaskDelete(task.handle);
        task.handle = nullptr;
        
        // Small delay to allow cleanup
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

bool TaskDependencyManager::defaultHealthCheck(const TaskInfo& task) {
    // Default check: task handle is valid and stack has reasonable margin
    if (task.handle == nullptr) {
        return false;
    }
    
    UBaseType_t stackHighWaterMark = uxTaskGetStackHighWaterMark(task.handle);
    if (stackHighWaterMark < 100) {  // Less than 100 words free
        LOG_WARN(TAG, "Task %s has low stack: %d words", task.name.c_str(), stackHighWaterMark);
        return false;
    }
    
    return true;
}

void TaskDependencyManager::healthMonitorTaskFunction(void* pvParameters) {
    TaskDependencyManager* manager = static_cast<TaskDependencyManager*>(pvParameters);
    const char* localTag = "HealthMonitor";
    
    LOG_INFO(localTag, "Task health monitoring started");
    
    while (true) {
        // Perform health checks
        uint8_t unhealthyCount = manager->performHealthCheck();
        
        if (unhealthyCount > 0) {
            LOG_WARN(localTag, "Found %d unhealthy tasks", unhealthyCount);
        }
        
        vTaskDelay(pdMS_TO_TICKS(manager->healthCheckIntervalMs));
    }
}

void TaskDependencyManager::handleTaskFailure(const std::string& taskName) {
    LOG_ERROR(TAG, "Task failed health check: %s", taskName.c_str());
    
    if (!autoRestartEnabled) {
        updateTaskState(taskName, TaskState::FAILED);
        return;
    }
    
    // Check restart count
    if (xSemaphoreTake(mutex, MUTEX_TIMEOUT) == pdTRUE) {
        auto it = tasks.find(taskName);
        if (it != tasks.end() && it->second.restartCount < maxRestartAttempts) {
            it->second.restartCount++;
            xSemaphoreGive(mutex);
            
            // Attempt restart
            if (restartTask(taskName)) {
                LOG_INFO(TAG, "Successfully restarted task: %s", taskName.c_str());
            } else {
                updateTaskState(taskName, TaskState::FAILED);
                LOG_ERROR(TAG, "Failed to restart task: %s", taskName.c_str());
            }
        } else {
            xSemaphoreGive(mutex);
            updateTaskState(taskName, TaskState::FAILED);
            LOG_ERROR(TAG, "Max restart attempts reached for task: %s", taskName.c_str());
        }
    }
}

void TaskDependencyManager::updateTaskState(const std::string& taskName, TaskState newState) {
    if (xSemaphoreTake(mutex, MUTEX_TIMEOUT) == pdTRUE) {
        auto it = tasks.find(taskName);
        if (it != tasks.end()) {
            TaskState oldState = it->second.state;
            it->second.state = newState;
            
            if (oldState != newState) {
                notifyStateChange(taskName, newState);
            }
        }
        xSemaphoreGive(mutex);
    }
}

void TaskDependencyManager::notifyStateChange(const std::string& taskName, TaskState newState) {
    // Log state change
    const char* stateStr = "UNKNOWN";
    switch (newState) {
        case TaskState::NOT_STARTED: stateStr = "NOT_STARTED"; break;
        case TaskState::STARTING: stateStr = "STARTING"; break;
        case TaskState::RUNNING: stateStr = "RUNNING"; break;
        case TaskState::FAILED: stateStr = "FAILED"; break;
        case TaskState::STOPPED: stateStr = "STOPPED"; break;
        case TaskState::RESTARTING: stateStr = "RESTARTING"; break;
    }
    
    LOG_INFO(TAG, "Task %s state changed to: %s", taskName.c_str(), stateStr);
    
    // Set event bits for state change notification
    if (stateEventGroup) {
        xEventGroupSetBits(stateEventGroup, (1 << (int)newState));
    }
}