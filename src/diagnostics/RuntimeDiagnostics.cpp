// src/diagnostics/RuntimeDiagnostics.cpp
#include "RuntimeDiagnostics.h"
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdarg>
#include <cstdio>
#include "shared/SharedResources.h"
#include "shared/SharedSensorReadings.h"
#include "shared/SharedRelayReadings.h"
#include "EthernetManager.h"
#include "monitoring/HealthMonitor.h"
#include "core/SystemResourceProvider.h"

// No external declarations needed - using SRP methods

// Static members
bool RuntimeDiagnostics::initialized_ = false;
bool RuntimeDiagnostics::verboseMode_ = false;
bool RuntimeDiagnostics::tracingEnabled_ = false;
RuntimeDiagnostics::OutputCallback RuntimeDiagnostics::currentOutput_ = nullptr;

void RuntimeDiagnostics::initialize() {
    initialized_ = true;
    LOG_INFO("DIAG", "Runtime diagnostics initialized");
}

void RuntimeDiagnostics::executeCommand(DiagnosticCommand cmd, OutputCallback outputCb) {
    currentOutput_ = outputCb;
    
    switch (cmd) {
        case DiagnosticCommand::DUMP_TASKS:
            dumpTaskInfo(outputCb);
            break;
        case DiagnosticCommand::DUMP_MEMORY:
            dumpMemoryInfo(outputCb);
            break;
        case DiagnosticCommand::DUMP_EVENT_GROUPS:
            dumpEventGroups(outputCb);
            break;
        case DiagnosticCommand::DUMP_MUTEXES:
            dumpMutexInfo(outputCb);
            break;
        case DiagnosticCommand::DUMP_SENSORS:
            dumpSensorInfo(outputCb);
            break;
        case DiagnosticCommand::DUMP_RELAYS:
            dumpRelayInfo(outputCb);
            break;
        case DiagnosticCommand::DUMP_NETWORK:
            dumpNetworkInfo(outputCb);
            break;
        case DiagnosticCommand::DUMP_MODBUS:
            dumpModbusInfo(outputCb);
            break;
        case DiagnosticCommand::TRACE_ENABLE:
            enableTracing();
            output("Execution tracing enabled");
            break;
        case DiagnosticCommand::TRACE_DISABLE:
            disableTracing();
            output("Execution tracing disabled");
            break;
        case DiagnosticCommand::RUN_SELF_TEST:
            runSelfTest(outputCb);
            break;
        case DiagnosticCommand::FORCE_REBOOT:
            output("Rebooting system in 3 seconds...");
            vTaskDelay(pdMS_TO_TICKS(3000));
            ESP.restart();
            break;
        case DiagnosticCommand::DUMP_ALL:
            output("=== COMPLETE SYSTEM DIAGNOSTICS ===");
            dumpTaskInfo(outputCb);
            dumpMemoryInfo(outputCb);
            dumpEventGroups(outputCb);
            dumpSensorInfo(outputCb);
            dumpRelayInfo(outputCb);
            dumpNetworkInfo(outputCb);
            output("=== END DIAGNOSTICS ===");
            break;
        default:
            output("Unknown diagnostic command");
            break;
    }
    
    currentOutput_ = nullptr;
}

void RuntimeDiagnostics::dumpTaskInfo(OutputCallback outputCb) {
    output("=== Task Information ===");
    
    // Get number of tasks
    UBaseType_t taskCount = uxTaskGetNumberOfTasks();
    output("Total tasks: %d", taskCount);
    
    // Allocate array for task status
    TaskStatus_t* taskStatusArray = (TaskStatus_t*)pvPortMalloc(taskCount * sizeof(TaskStatus_t));
    if (taskStatusArray == nullptr) {
        output("Failed to allocate memory for task status");
        return;
    }
    
    // Get task status
    uint32_t totalRunTime = 0;
    taskCount = uxTaskGetSystemState(taskStatusArray, taskCount, &totalRunTime);
    
    // Sort by stack usage (lowest first)
    std::sort(taskStatusArray, taskStatusArray + taskCount,
              [](const TaskStatus_t& a, const TaskStatus_t& b) {
                  return a.usStackHighWaterMark < b.usStackHighWaterMark;
              });
    
    // Display task information
    output("%-16s %5s %8s %10s %6s", "Task Name", "State", "Priority", "Stack Free", "Core");
    output("%-16s %5s %8s %10s %6s", "--------", "-----", "--------", "----------", "----");
    
    for (UBaseType_t i = 0; i < taskCount; i++) {
        const char* stateStr;
        switch (taskStatusArray[i].eCurrentState) {
            case eReady: stateStr = "Ready"; break;
            case eBlocked: stateStr = "Block"; break;
            case eSuspended: stateStr = "Susp"; break;
            case eDeleted: stateStr = "Del"; break;
            default: stateStr = "?"; break;
        }
        
        int coreId = xTaskGetCoreID(taskStatusArray[i].xHandle);
        
        output("%-16s %5s %8d %10d %6d",
               taskStatusArray[i].pcTaskName,
               stateStr,
               taskStatusArray[i].uxCurrentPriority,
               taskStatusArray[i].usStackHighWaterMark * sizeof(StackType_t),
               coreId);
               
        // Warn about low stack
        if (taskStatusArray[i].usStackHighWaterMark < 256 / sizeof(StackType_t)) {
            output("  WARNING: Low stack!");
        }
    }
    
    vPortFree(taskStatusArray);
    output("");
}

void RuntimeDiagnostics::dumpMemoryInfo(OutputCallback outputCb) {
    output("=== Memory Information ===");
    
    // Heap information
    output("Free Heap: %s / %s (%lu%%)",
           formatBytes(ESP.getFreeHeap()).c_str(),
           formatBytes(ESP.getHeapSize()).c_str(),
           (ESP.getFreeHeap() * 100UL) / ESP.getHeapSize());
           
    output("Min Free Heap: %s", formatBytes(ESP.getMinFreeHeap()).c_str());
    output("Max Alloc Heap: %s", formatBytes(ESP.getMaxAllocHeap()).c_str());
    
    // PSRAM information (if available)
    if (ESP.getPsramSize() > 0) {
        output("Free PSRAM: %s / %s (%lu%%)",
               formatBytes(ESP.getFreePsram()).c_str(),
               formatBytes(ESP.getPsramSize()).c_str(),
               (ESP.getFreePsram() * 100UL) / ESP.getPsramSize());
    }
    
    // Memory capabilities
    output("\nMemory by Type:");
    output("  DRAM: %s free", formatBytes(heap_caps_get_free_size(MALLOC_CAP_8BIT)).c_str());
    output("  IRAM: %s free", formatBytes(heap_caps_get_free_size(MALLOC_CAP_32BIT)).c_str());
    output("  Largest free block: %s", formatBytes(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)).c_str());
    
    // Memory fragmentation estimate
    size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    size_t freeHeap = ESP.getFreeHeap();
    // Calculate fragmentation as percentage (100 - (largestBlock * 100 / freeHeap))
    // Use 64-bit arithmetic to prevent overflow on large heap values
    uint32_t fragmentation = freeHeap > 0 ?
        100 - static_cast<uint32_t>((static_cast<uint64_t>(largestBlock) * 100ULL) / freeHeap) : 0;
    output("  Fragmentation: %lu%%", fragmentation);
    
    output("");
}

void RuntimeDiagnostics::dumpEventGroups(OutputCallback outputCb) {
    output("=== Event Group States ===");
    
    // Define event group names and their handles
    struct EventGroupInfo {
        EventGroupHandle_t handle;
        const char* name;
    } eventGroups[] = {
        {SRP::getSensorEventGroup(), "SensorEvent"},
        {SRP::getRelayEventGroup(), "RelayEvent"},
        // SYSTEM removed - consolidated into SYSTEM_STATE
        {SRP::getSystemStateEventGroup(), "SystemState"},
        {SRP::getBurnerEventGroup(), "BurnerEvent"},
        {SRP::getHeatingEventGroup(), "HeatingEvent"},
        // WHEATER removed - unused
        {SRP::getControlRequestsEventGroup(), "ControlRequests"},
        {SRP::getErrorNotificationEventGroup(), "ErrorNotification"},
        // TIMER removed - unused
        {SRP::getRelayStatusEventGroup(), "RelayStatus"},
        // {SRP::getSensorMiThEventGroup(), "SensorMiTh"}  // BLE removed
    };
    
    for (const auto& eg : eventGroups) {
        if (eg.handle != nullptr) {
            EventBits_t bits = xEventGroupGetBits(eg.handle);
            output("%-20s: 0x%08X (%s)", eg.name, bits, decodeEventBits(eg.handle, bits).c_str());
        } else {
            output("%-20s: Not initialized", eg.name);
        }
    }
    
    output("");
}

void RuntimeDiagnostics::dumpSensorInfo(OutputCallback outputCb) {
    output("=== Sensor Information ===");
    
    // Get sensor readings with mutex
    if (SRP::takeSensorReadingsMutex(pdMS_TO_TICKS(100)) == pdTRUE) {
        output("Temperature Sensors:");
        
        // Boiler temperatures
        if (SRP::getSensorReadings().isBoilerTempOutputValid) {
            output("  Boiler Output: %.1f°C", SRP::getSensorReadings().boilerTempOutput);
        } else {
            output("  Boiler Output: INVALID");
        }
        
        if (SRP::getSensorReadings().isBoilerTempReturnValid) {
            output("  Boiler Return: %.1f°C", SRP::getSensorReadings().boilerTempReturn);
        } else {
            output("  Boiler Return: INVALID");
        }
        
        // Room temperature
        if (SRP::getSensorReadings().isInsideTempValid) {
            output("  Inside Temp: %.1f°C", SRP::getSensorReadings().insideTemp);
            output("  Inside Humidity: %.1f%%", SRP::getSensorReadings().insideHumidity);
        } else {
            output("  Inside Temp: INVALID");
        }
        
        // Outside temperature
        if (SRP::getSensorReadings().isOutsideTempValid) {
            output("  Outside Temp: %.1f°C", SRP::getSensorReadings().outsideTemp);
        } else {
            output("  Outside Temp: INVALID");
        }
        
        // Water temperatures
        if (SRP::getSensorReadings().isWaterHeaterTempTankValid) {
            output("  Water Heater Tank Temp: %.1f°C", SRP::getSensorReadings().waterHeaterTempTank);
        } else {
            output("  Water Heater Tank Temp: INVALID");
        }
        
        SRP::giveSensorReadingsMutex();
    } else {
        output("Failed to acquire sensor readings mutex");
    }
    
    output("");
}

void RuntimeDiagnostics::dumpRelayInfo(OutputCallback outputCb) {
    output("=== Relay Information ===");
    
    // Get relay states
    if (SRP::takeRelayReadingsMutex(pdMS_TO_TICKS(100)) == pdTRUE) {
        output("Relay States:");
        
        for (int i = 0; i < 8; i++) {
            // Note: Adjust based on actual SharedRelayReadings structure
            output("  Relay %d: %s", i + 1, "Unknown");
        }
        
        SRP::giveRelayReadingsMutex();
    } else {
        output("Failed to acquire relay readings mutex");
    }
    
    output("");
}

void RuntimeDiagnostics::dumpNetworkInfo(OutputCallback outputCb) {
    output("=== Network Information ===");
    
    if (EthernetManager::isConnected()) {
        output("Ethernet: Connected");
        output("  IP Address: %s", ETH.localIP().toString().c_str());
        output("  Subnet Mask: %s", ETH.subnetMask().toString().c_str());
        output("  Gateway: %s", ETH.gatewayIP().toString().c_str());
        output("  DNS: %s", ETH.dnsIP().toString().c_str());
        output("  MAC Address: %s", ETH.macAddress().c_str());
        output("  Link Speed: %d Mbps", ETH.linkSpeed());
        output("  Full Duplex: %s", ETH.fullDuplex() ? "Yes" : "No");
    } else {
        output("Ethernet: Disconnected");
    }
    
    // Get health monitor network metrics
    if (SRP::getHealthMonitor() != nullptr) {
        auto metrics = SRP::getHealthMonitor()->getNetworkMetrics();
        output("\nNetwork Statistics:");
        output("  Disconnections: %d", metrics.disconnectCount);
        output("  Reconnections: %d", metrics.reconnectCount);
        // Convert fixed-point (10000 = 100%) to display: XX.X%
        output("  Availability: %u.%u%%", metrics.availabilityFP / 100, (metrics.availabilityFP % 100) / 10);
    }
    
    output("");
}

void RuntimeDiagnostics::enableTracing() {
    tracingEnabled_ = true;
}

void RuntimeDiagnostics::disableTracing() {
    tracingEnabled_ = false;
}

RuntimeDiagnostics::FunctionTracer::FunctionTracer(const char* function, const char* file, int line) 
    : function_(function), entryTime_(millis()) {
    if (RuntimeDiagnostics::tracingEnabled_) {
        LOG_DEBUG("TRACE", "ENTER %s (%s:%d)", function_, file, line);
    }
}

RuntimeDiagnostics::FunctionTracer::~FunctionTracer() {
    if (RuntimeDiagnostics::tracingEnabled_) {
        uint32_t duration = millis() - entryTime_;
        // Cast to void to suppress unused variable warning when LOG_DEBUG is optimized out
        (void)duration;
        LOG_DEBUG("TRACE", "EXIT %s (took %lu ms)", function_, (unsigned long)duration);
    }
}

void RuntimeDiagnostics::output(const std::string& message) {
    if (currentOutput_) {
        currentOutput_(message);
    } else {
        LOG_INFO("DIAG", "%s", message.c_str());
    }
}

void RuntimeDiagnostics::output(const char* format, ...) {
    // Use a smaller buffer to reduce stack usage
    char buffer[128];  // Reduced from 256
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    output(std::string(buffer));
}

std::string RuntimeDiagnostics::formatBytes(size_t bytes) {
    char buffer[32];
    if (bytes >= 1024 * 1024) {
        // Integer math: MB with 1 decimal place
        uint32_t mb = bytes / (1024 * 1024);
        uint32_t frac = ((bytes % (1024 * 1024)) * 10) / (1024 * 1024);
        snprintf(buffer, sizeof(buffer), "%lu.%lu MB", (unsigned long)mb, (unsigned long)frac);
    } else if (bytes >= 1024) {
        // Integer math: KB with 1 decimal place
        uint32_t kb = bytes / 1024;
        uint32_t frac = ((bytes % 1024) * 10) / 1024;
        snprintf(buffer, sizeof(buffer), "%lu.%lu KB", (unsigned long)kb, (unsigned long)frac);
    } else {
        snprintf(buffer, sizeof(buffer), "%zu B", bytes);
    }
    return std::string(buffer);
}

std::string RuntimeDiagnostics::formatDuration(uint32_t ms) {
    char buffer[32];
    if (ms >= 60000) {
        snprintf(buffer, sizeof(buffer), "%lu min", (unsigned long)(ms / 60000));
    } else if (ms >= 1000) {
        // Convert to seconds with one decimal place using integer math
        uint32_t seconds = ms / 1000;
        uint32_t tenths = (ms % 1000) / 100;
        snprintf(buffer, sizeof(buffer), "%lu.%lu sec", (unsigned long)seconds, (unsigned long)tenths);
    } else {
        snprintf(buffer, sizeof(buffer), "%lu ms", (unsigned long)ms);
    }
    return std::string(buffer);
}

std::string RuntimeDiagnostics::decodeEventBits(EventGroupHandle_t handle, EventBits_t bits) {
    // THREAD-SAFETY: Static buffer safe - called from MQTT task only (diagnostics context)
    // Alternative: Heap allocation (std::string construction) or 80B on already-tight MQTT stack
    // See: docs/MEMORY_OPTIMIZATION.md
    // Max: 24 bits, each "XX," is 3 chars max = 72 chars + null
    static char buffer[80];

    if (bits == 0) {
        return "none";
    }

    char* ptr = buffer;
    char* end = buffer + sizeof(buffer) - 1;
    bool first = true;

    for (int i = 0; i < 24 && ptr < end; i++) {  // Event groups use 24 bits
        if (bits & (1 << i)) {
            if (!first && ptr < end) {
                *ptr++ = ',';
            }
            int written = snprintf(ptr, end - ptr, "%d", i);
            if (written > 0) ptr += written;
            first = false;
        }
    }
    *ptr = '\0';

    return std::string(buffer);
}

// Stub implementations
void RuntimeDiagnostics::dumpMutexInfo(OutputCallback outputCb) {
    output("=== Mutex Information ===");
    output("Mutex diagnostics not yet implemented");
    output("");
}

void RuntimeDiagnostics::dumpModbusInfo(OutputCallback outputCb) {
    output("=== Modbus Information ===");
    output("Modbus diagnostics not yet implemented");
    output("");
}

void RuntimeDiagnostics::runSelfTest(OutputCallback outputCb) {
    output("=== Running Self-Test ===");
    output("Self-test not yet implemented");
    output("");
}

std::string RuntimeDiagnostics::getHelpText() {
    return R"(
Runtime Diagnostics Commands:
  tasks      - Display task information
  memory     - Display memory statistics
  events     - Display event group states
  sensors    - Display sensor readings
  relays     - Display relay states
  network    - Display network status
  trace on   - Enable execution tracing
  trace off  - Disable execution tracing
  all        - Display all diagnostic information
  reboot     - Reboot the system
  help       - Display this help text
)";
}