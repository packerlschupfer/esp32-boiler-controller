// src/diagnostics/RuntimeDiagnostics.h
#ifndef RUNTIME_DIAGNOSTICS_H
#define RUNTIME_DIAGNOSTICS_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdint>
#include <string>
#include <functional>
#include "config/ProjectConfig.h"

/**
 * @brief Runtime diagnostics for debugging and troubleshooting
 * 
 * Provides various diagnostic functions that can be triggered at runtime
 * to help debug system issues without requiring recompilation.
 */
class RuntimeDiagnostics {
public:
    /**
     * @brief Diagnostic command types
     */
    enum class DiagnosticCommand {
        DUMP_TASKS,           // Dump all task information
        DUMP_MEMORY,          // Dump memory statistics
        DUMP_EVENT_GROUPS,    // Dump event group states
        DUMP_MUTEXES,         // Dump mutex information
        DUMP_SENSORS,         // Dump sensor readings
        DUMP_RELAYS,          // Dump relay states
        DUMP_NETWORK,         // Dump network status
        DUMP_MODBUS,          // Dump Modbus statistics
        TRACE_ENABLE,         // Enable execution tracing
        TRACE_DISABLE,        // Disable execution tracing
        SIMULATE_ERROR,       // Simulate an error condition
        TRIGGER_WATCHDOG,     // Trigger watchdog timeout
        FORCE_REBOOT,         // Force system reboot
        RUN_SELF_TEST,        // Run self-test sequence
        DUMP_ALL              // Dump all diagnostic info
    };
    
    /**
     * @brief Diagnostic output callback
     */
    using OutputCallback = std::function<void(const std::string&)>;
    
    /**
     * @brief Initialize diagnostics
     */
    static void initialize();
    
    /**
     * @brief Execute a diagnostic command
     */
    static void executeCommand(DiagnosticCommand cmd, OutputCallback output = nullptr);
    
    /**
     * @brief Execute a diagnostic command by string
     */
    static void executeCommand(const std::string& cmdStr, OutputCallback output = nullptr);
    
    /**
     * @brief Enable/disable verbose diagnostics
     */
    static void setVerbose(bool verbose) { verboseMode_ = verbose; }
    
    /**
     * @brief Dump task information
     */
    static void dumpTaskInfo(OutputCallback output = nullptr);
    
    /**
     * @brief Dump memory information
     */
    static void dumpMemoryInfo(OutputCallback output = nullptr);
    
    /**
     * @brief Dump event group states
     */
    static void dumpEventGroups(OutputCallback output = nullptr);
    
    /**
     * @brief Dump mutex information
     */
    static void dumpMutexInfo(OutputCallback output = nullptr);
    
    /**
     * @brief Dump sensor readings
     */
    static void dumpSensorInfo(OutputCallback output = nullptr);
    
    /**
     * @brief Dump relay states
     */
    static void dumpRelayInfo(OutputCallback output = nullptr);
    
    /**
     * @brief Dump network status
     */
    static void dumpNetworkInfo(OutputCallback output = nullptr);
    
    /**
     * @brief Dump Modbus statistics
     */
    static void dumpModbusInfo(OutputCallback output = nullptr);
    
    /**
     * @brief Enable execution tracing
     */
    static void enableTracing();
    
    /**
     * @brief Disable execution tracing
     */
    static void disableTracing();
    
    /**
     * @brief Trace function entry/exit
     */
    class FunctionTracer {
    public:
        FunctionTracer(const char* function, const char* file, int line);
        ~FunctionTracer();
    private:
        const char* function_;
        uint32_t entryTime_;
    };
    
    /**
     * @brief Run self-test sequence
     */
    static void runSelfTest(OutputCallback output = nullptr);
    
    /**
     * @brief Get diagnostic help text
     */
    static std::string getHelpText();
    
    /**
     * @brief Register custom diagnostic command
     */
    static void registerCommand(const std::string& name, 
                               std::function<void(OutputCallback)> handler);
    
private:
    static bool initialized_;
    static bool verboseMode_;
    static bool tracingEnabled_;
    static OutputCallback currentOutput_;
    
    // Helper to output diagnostic message
    static void output(const std::string& message);
    static void output(const char* format, ...);
    
    // Format helpers
    static std::string formatBytes(size_t bytes);
    static std::string formatDuration(uint32_t ms);
    static std::string formatPercentage(float value);
    
    // Event group names (should match your system)
    static const char* getEventGroupName(EventGroupHandle_t handle);
    static std::string decodeEventBits(EventGroupHandle_t handle, EventBits_t bits);
};

// Diagnostic macros
#ifdef DEBUG_BUILD

#define DIAG_TRACE() \
    RuntimeDiagnostics::FunctionTracer _tracer(__FUNCTION__, __FILE__, __LINE__)

#define DIAG_LOG(format, ...) \
    if (RuntimeDiagnostics::isVerbose()) { \
        LOG_DEBUG("DIAG", format, ##__VA_ARGS__); \
    }

#else

#define DIAG_TRACE()
#define DIAG_LOG(format, ...)

#endif

// Console command handler integration
void handleDiagnosticCommand(const std::string& cmd, const std::string& args);

#endif // RUNTIME_DIAGNOSTICS_H