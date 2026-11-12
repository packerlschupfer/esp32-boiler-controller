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
#include "shared/RelayState.h"              // g_relayState - the relay source of truth
#include "config/RelayIndices.h"              // relay names, so a re-map moves the labels
#include "modules/diagnostics/TestMode.h"     // a manual window explains a surprising mask
#include "EthernetManager.h"
#include "monitoring/HealthMonitor.h"
#include "core/SystemResourceProvider.h"
#include "config/SystemConstants.h"

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
        
        // tskNO_AFFINITY is INT_MAX, which printed as a raw "2147483647" in the Core
        // column for every unpinned task (HeatingControl, ControlTask, WheaterControl,
        // emac_rx, Tmr Svc). MonitoringTask already renders it as "ANY"; match that.
        const int coreId = xTaskGetCoreID(taskStatusArray[i].xHandle);
        char coreStr[12];  // worst-case "%d" of an int is 11 chars + NUL
        if (coreId == tskNO_AFFINITY) {
            snprintf(coreStr, sizeof(coreStr), "ANY");
        } else {
            snprintf(coreStr, sizeof(coreStr), "%d", coreId);
        }

        output("%-16s %5s %8u %10u %6s",
               taskStatusArray[i].pcTaskName,
               stateStr,
               static_cast<unsigned>(taskStatusArray[i].uxCurrentPriority),
               static_cast<unsigned>(taskStatusArray[i].usStackHighWaterMark *
                                     sizeof(StackType_t)),
               coreStr);
               
        // Warn about low stack
        // Same threshold as MonitoringTask::logAllTasks(); this site used 256 B, so a
        // task could read low in one report and fine in the other. Framework-owned stacks
        // are excluded from the WARNING for the reason given at isFrameworkOwnedStack();
        // they are still listed above with their real figures.
        if (taskStatusArray[i].usStackHighWaterMark * sizeof(StackType_t) <
                SystemConstants::System::LOW_STACK_WARN_BYTES &&
            !SystemConstants::System::isFrameworkOwnedStack(taskStatusArray[i].pcTaskName)) {
            output("  WARNING: Low stack!");
        }
    }
    
    vPortFree(taskStatusArray);
    output(std::string());
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
    
    // Memory capabilities.
    //
    // These are the numbers that matter: ESP.getFreeHeap()/getMaxAllocHeap() above are
    // MALLOC_CAP_INTERNAL, which counts IRAM-only memory that malloc() cannot serve. On
    // 2026-09-16 this device reported 56.9 kB "free" with a 47.9 kB "largest block" while
    // byte-addressable DRAM was 7.0 kB with a 4.2 kB ceiling - which is why an OTA-time
    // malloc(1436) could fail. MALLOC_CAP_DEFAULT is what plain malloc() uses.
    output("\nMemory by Type:");
    output("  DRAM (8BIT): %s free", formatBytes(heap_caps_get_free_size(MALLOC_CAP_8BIT)).c_str());
    output("  IRAM (32BIT): %s free", formatBytes(heap_caps_get_free_size(MALLOC_CAP_32BIT)).c_str());
    output("  malloc (DEFAULT): %s free", formatBytes(heap_caps_get_free_size(MALLOC_CAP_DEFAULT)).c_str());
    output("  esp_get_free_heap_size: %s", formatBytes(esp_get_free_heap_size()).c_str());
    output("  DRAM min since boot: %s",
           formatBytes(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)).c_str());
    output("  Largest free block: %s", formatBytes(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)).c_str());
    
    // Memory fragmentation estimate
    size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    size_t freeHeap = ESP.getFreeHeap();
    // Calculate fragmentation as percentage (100 - (largestBlock * 100 / freeHeap))
    // Use 64-bit arithmetic to prevent overflow on large heap values
    uint32_t fragmentation = freeHeap > 0 ?
        100 - static_cast<uint32_t>((static_cast<uint64_t>(largestBlock) * 100ULL) / freeHeap) : 0;
    output("  Fragmentation: %lu%%", fragmentation);
    
    output(std::string());
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
            output("%-20s: 0x%08lX (%s)", eg.name, static_cast<unsigned long>(bits),
                   decodeEventBits(eg.handle, bits).c_str());
        } else {
            output("%-20s: Not initialized", eg.name);
        }
    }
    
    output(std::string());
}

void RuntimeDiagnostics::dumpSensorInfo(OutputCallback outputCb) {
    output("=== Sensor Information ===");

    // Snapshot under the mutex, then format and publish OUTSIDE it.
    //
    // The previous version held sensorReadingsMutex across every output() call. output()
    // can trigger flush(), which publishes and then paces with vTaskDelay - so a dump that
    // grew past one 320-byte chunk would put a delay inside a mutex that control tasks
    // acquire with a 100 ms timeout. The sensors text measured ~223 B of 320, i.e. one
    // extra sensor line away from that. dumpRelayInfo has the same shape.
    struct Snap {
        Temperature_t boilerOut, boilerRet, inside, outside, tank;
        float humidity;
        bool boilerOutOk, boilerRetOk, insideOk, outsideOk, tankOk, humidityOk;
    } snap{};

    if (SRP::takeSensorReadingsMutex(pdMS_TO_TICKS(100)) != pdTRUE) {
        output("Failed to acquire sensor readings mutex");
        output(std::string());
        return;
    }
    {
        const SharedSensorReadings& r = SRP::getSensorReadings();
        snap = {r.boilerTempOutput, r.boilerTempReturn, r.insideTemp, r.outsideTemp,
                r.waterHeaterTempTank, r.insideHumidity,
                r.isBoilerTempOutputValid, r.isBoilerTempReturnValid, r.isInsideTempValid,
                r.isOutsideTempValid, r.isWaterHeaterTempTankValid, r.isInsideHumidityValid};
    }
    SRP::giveSensorReadingsMutex();

    output("Temperature Sensors:");
    // formatTemp() from shared/Temperature.h, not a local helper: it does the same integer
    // tenths formatting (no %f, so no _dtoa_r/_Balloc on this task) AND maps TEMP_INVALID
    // to "N/A". A local copy of it here printed TEMP_INVALID as "-3276.8" - inventing a
    // precise-looking reading, the exact failure this dump was being fixed for.
    char t[12];
    auto emit = [&](const char* label, Temperature_t v, bool valid) {
        if (!valid) {
            output("  %s: INVALID", label);
            return;
        }
        formatTemp(t, sizeof(t), v);
        output("  %s: %s°C", label, t);
    };

    emit("Boiler Output", snap.boilerOut, snap.boilerOutOk);
    emit("Boiler Return", snap.boilerRet, snap.boilerRetOk);
    emit("Inside Temp", snap.inside, snap.insideOk);
    if (snap.insideOk) {
        // ANDRTF3 has no humidity channel (ANDRTF3Task sets isInsideHumidityValid = false)
        // and nothing writes through SensorBindings' insideHumidityPtr, so this reported a
        // fabricated 0.0% until the flag was honoured. Kept integer-formatted for the same
        // reason as the temperatures.
        if (snap.humidityOk) {
            const int whole = static_cast<int>(snap.humidity);
            const int frac = static_cast<int>((snap.humidity - whole) * 10.0f + 0.5f);
            output("  Inside Humidity: %d.%d%%", whole, frac);
        } else {
            output("  Inside Humidity: N/A (no humidity sensor)");
        }
    }
    emit("Outside Temp", snap.outside, snap.outsideOk);
    emit("Water Heater Tank Temp", snap.tank, snap.tankOk);

    output(std::string());
}

void RuntimeDiagnostics::dumpRelayInfo(OutputCallback outputCb) {
    output("=== Relay Information ===");

    // THIS USED TO BE A STUB. It took the relay-readings mutex, looped eight times printing
    // the literal string "Unknown", and carried a `// Note: Adjust based on actual
    // SharedRelayReadings structure` TODO. So `boiler/cmd/diagnostics relays` has answered
    // "Relay 1..8: Unknown" for the life of the project while looking like a working report.
    //
    // Found 2026-11-13 while testing manual test mode, which is what makes it matter: only
    // five of the eight relays appear in the published `r` bitmask on boiler/status/sensors
    // (burner, heating pump, water pump, power boost, water mode). VALVE, SPARE and ALARM are
    // published NOWHERE, so this report was the only way to see them - and it did not.
    //
    // It now reads g_relayState, which is the single source of truth that RYN4ProcessingTask
    // actually transmits and renews - NOT SharedRelayReadings, which is a lossy second copy
    // with no alarm field at all and relay numbers that disagree with RelayIndex in its own
    // comments. The relay-readings mutex is therefore no longer taken: nothing read here
    // needs it, and these are atomics.
    //
    // DESIRED / SENT / ACTUAL are all three printed on purpose, because the difference between
    // them IS the diagnosis. desired != sent means a write is queued for the next RYN4_SET
    // tick; sent != actual means the hardware did not follow, which is what
    // RelayVerificationManager escalates on after MAX_CONSECUTIVE_FAILURES.
    const uint8_t desired = g_relayState.desired.load(std::memory_order_acquire);
    const uint8_t sent    = g_relayState.sent.load(std::memory_order_acquire);
    const uint8_t actual  = g_relayState.actual.load(std::memory_order_acquire);

    output("Masks: desired=0x%02X sent=0x%02X actual=0x%02X%s", desired, sent, actual,
           (sent != actual) ? "  <-- HARDWARE DID NOT FOLLOW" : "");

    // Indexed assignment, NOT a designated-initialiser array. The first version wrote
    // `[RelayIndex::VALVE] = "valve"` and friends - a C99 feature GCC happens to accept in
    // C++, and one that silently requires the index constants to be in INCREASING order. So
    // the moment someone re-maps the relays into a different order - the exact event these
    // labels exist to survive - the labels would land on the wrong bits, in a build that
    // still compiled. Assigning by index is order-independent, and the "?" prefill means a
    // relay added to RelayIndex without a label here prints "?" rather than a stale pointer.
    const char* names[RelayIndex::MAX_RELAYS];
    for (uint8_t i = 0; i < RelayIndex::MAX_RELAYS; i++) {
        names[i] = "?";
    }
    names[RelayIndex::BURNER_ENABLE] = "burner_enable";
    names[RelayIndex::POWER_BOOST]   = "power_boost";
    names[RelayIndex::WATER_MODE]    = "water_mode";
    names[RelayIndex::VALVE]         = "valve";
    names[RelayIndex::HEATING_PUMP]  = "heating_pump";
    names[RelayIndex::WATER_PUMP]    = "water_pump";
    names[RelayIndex::SPARE_7]       = "spare";
    names[RelayIndex::ALARM]         = "alarm";

    output("Relay States (physical: name = actual, desired if different):");
    for (uint8_t i = 0; i < RelayIndex::MAX_RELAYS; i++) {
        const uint8_t bit = static_cast<uint8_t>(1u << i);
        const bool a = (actual & bit) != 0;
        const bool d = (desired & bit) != 0;
        if (a == d) {
            output("  %u: %-13s %s", RelayIndex::toPhysical(i), names[i], a ? "ON" : "OFF");
        } else {
            output("  %u: %-13s %s (desired %s)", RelayIndex::toPhysical(i), names[i],
                   a ? "ON" : "OFF", d ? "ON" : "OFF");
        }
    }

    // Beside the masks because a manual window explains any state that looks wrong, and an
    // operator reading this report during one needs to know that before anything else.
    if (TestMode::isActive()) {
        output("MANUAL TEST MODE ACTIVE - %lu s left. Relays are under operator control; "
               "the automatic path has stood down.",
               static_cast<unsigned long>(TestMode::secondsLeft()));
    }

    output(std::string());
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
        output("  Disconnections: %lu", static_cast<unsigned long>(metrics.disconnectCount));
        output("  Reconnections: %lu", static_cast<unsigned long>(metrics.reconnectCount));
        // Convert fixed-point (10000 = 100%) to display: XX.X%
        output("  Availability: %lu.%lu%%",
               static_cast<unsigned long>(metrics.availabilityFP / 100),
               static_cast<unsigned long>((metrics.availabilityFP % 100) / 10));
    }
    
    output(std::string());
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
    output(std::string());
}

void RuntimeDiagnostics::dumpModbusInfo(OutputCallback outputCb) {
    output("=== Modbus Information ===");
    output("Modbus diagnostics not yet implemented");
    output(std::string());
}

void RuntimeDiagnostics::runSelfTest(OutputCallback outputCb) {
    output("=== Running Self-Test ===");
    output("Self-test not yet implemented");
    output(std::string());
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