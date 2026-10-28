// src/modules/mqtt/MQTTPublisher.cpp
#include "MQTTPublisher.h"
#include "modules/tasks/MQTTTask.h"
#include "core/SystemResourceProvider.h"
#include "core/ModbusBusRelease.h"  // bus-release expiry backstop and its status topic
#include "MQTTTopics.h"
#include "modules/tasks/MonitoringTask.h"
#include "modules/control/CentralizedFailsafe.h"  // centralized topic macros - see the note below
#include "shared/SharedSensorReadings.h"
#include "shared/SharedRelayReadings.h"
#include "shared/Temperature.h"
#include "events/SystemEventsGenerated.h"
#include "modules/control/TemperatureSensorFallback.h"
#include "modules/control/BurnerStateMachine.h"
#include "utils/MemoryPool.h"
#include <SemaphoreGuard.h>
#include <ArduinoJson.h>
#include <esp_log.h>
#include <Esp.h>
#include <esp_heap_caps.h>  // real malloc-servable heap, see publishSystemStatus()
#include <esp_system.h>

static const char* TAG = "MQTTPub";

// Topics come from MQTTTopics.h. This file used to redefine them as hardcoded literals
// ("avoid including MQTTTopics.h due to macro conflicts"), which is the very string drift
// that header exists to prevent - rename a topic centrally and this publisher would have
// kept publishing to the old one. Including it builds clean, so the duplicates are gone
// (2026-10-25). Both former values were byte-identical to the central ones.

namespace MQTTPublisher {

void publishSystemStatus() {
    // Backstop for the bus-release window, and the only place its end gets reported.
    //
    // Expiry normally happens in ModbusCoordinator::processTick(), which is the right place
    // because it is the one thing that can grant bus access. This second call covers the case
    // where the coordinator itself is stopped - then no tick would ever notice the deadline
    // and the window would never end. Cheap: two atomic loads when not released.
    //
    // The transition is logged and published HERE rather than at the point of expiry because
    // that point is a FreeRTOS timer callback, and a single LOG_* costs ~1312 B of stack on
    // this project.
    // Before the connectivity check below - but NOT unconditionally, which is what this comment
    // used to claim (2026-10-26 review). MQTTTask only calls publishSystemStatus() from inside
    // an isConnected() block (MQTTTask.cpp:451), so this backstop does not run while the broker
    // is unreachable.
    //
    // Benign, because it is the second line of defence: the real expiry is in
    // ModbusCoordinator::processTick(), which runs regardless of MQTT. This one covers "the
    // coordinator is stopped" only, and if the coordinator is stopped AND the broker is down
    // then nothing is polling anyway and the DELAY watchdog has already de-energised
    // everything. Worth knowing rather than believing the stronger claim.
    ModbusBusRelease::expireIfDue();

    auto mqttManager = SRP::getMQTTManager();
    if (!mqttManager || !mqttManager->isConnected()) {
        return;
    }

    // Only consumed once connected, so a transition that happens while the broker is down is
    // still reported on reconnect rather than swallowed by consumeStateChange().
    if (ModbusBusRelease::consumeStateChange()) {
        char busBuf[96];
        if (ModbusBusRelease::isReleased()) {
            snprintf(busBuf, sizeof(busBuf), "{\"released\":true,\"left\":%lu}",
                     static_cast<unsigned long>(ModbusBusRelease::secondsLeft()));
        } else {
            const bool timedOut = ModbusBusRelease::lastEnd() == ModbusBusRelease::Ended::BY_TIMEOUT;
            snprintf(busBuf, sizeof(busBuf), "{\"released\":false,\"end\":\"%s\"}",
                     timedOut ? "timeout" : "command");
            if (timedOut) {
                LOG_WARN(TAG, "RS485 bus release expired - polling resumed, burner re-armable");
            }
        }
        MQTTTask::publish(MQTT_STATUS_BUS, busBuf, 0, true, MQTTPriority::PRIORITY_HIGH);
    }

    // The failsafe level, which until 2026-10-25 was visible nowhere at all -
    // getFailsafeStatusString() had no callers. It matters now because the level is sticky and
    // DEGRADED withholds stage 2 for real, so a boiler left at half power has to be something
    // an operator can see. Retained, and consumed only while connected for the same reason the
    // bus state is: a transition during an outage must survive to the reconnect.
    // consumeLevelChange() clears the flag before the publish is known to have succeeded, so a
    // publish dropped by a full queue loses that transition until the NEXT level change
    // (2026-10-26 review). Accepted rather than fixed: the payload is retained, the level is
    // sticky, and the alternative - only consuming on a confirmed publish - needs a
    // publish-result path that MQTTTask::publish() does not expose. The cost is a stale
    // retained level until the next transition, which the periodic health topic would not
    // mask; if that ever matters, republish on reconnect rather than reworking the flag.
    if (CentralizedFailsafe::consumeLevelChange()) {
        char fsBuf[96];
        snprintf(fsBuf, sizeof(fsBuf), "{\"level\":\"%s\",\"fullPower\":%s}",
                 CentralizedFailsafe::getFailsafeStatusString(),
                 CentralizedFailsafe::fullPowerPermitted() ? "true" : "false");
        MQTTTask::publish(MQTT_STATUS_FAILSAFE, fsBuf, 0, true, MQTTPriority::PRIORITY_HIGH);
    }

    auto mqttMutex = SRP::getMQTTMutex();
    SemaphoreGuard guard(mqttMutex, pdMS_TO_TICKS(100));
    if (!guard.hasLock()) {
        LOG_ERROR(TAG, "Failed to acquire mutex for status publish");
        return;
    }

    // Create JSON status message
    JsonDocument doc;  // ArduinoJson v7

    // `timestamp` (raw millis()) was REMOVED here 2026-10-28. It was the same clock as
    // `uptime` below - millis() against millis()/1000 - and both grow in digits, so this
    // payload grew with uptime toward a hard cliff: measured at 250 B of a 256 B pooled
    // LogBuffer, reaching exactly 256 at 1,000,000 s (11.57 days), when timestamp hits 10
    // digits and uptime 7. At that point `written >= sizeof(buffer->data)` below drops the
    // WHOLE health publish, silently, until a reboot - and adding stack_min_task (~26 B) is
    // what moved that cliff from out of reach into range. Nothing consumed it: no HA entity
    // references health.timestamp, and `uptime` carries the same information in the unit a
    // reader actually wants. Dropping a redundant field is the right fix rather than growing
    // a pooled buffer that three other callers share.

    uint32_t heapFree = ESP.getFreeHeap();
    uint32_t heapMaxBlock = ESP.getMaxAllocHeap();
    doc["heap_free"] = heapFree;
    doc["heap_min"] = ESP.getMinFreeHeap();
    doc["heap_max_blk"] = heapMaxBlock;  // Largest free block (for fragmentation analysis)

    // heap_free/heap_max_blk above are MALLOC_CAP_INTERNAL (Esp.cpp), which counts
    // IRAM-only memory that malloc() cannot serve, so they read far healthier than the
    // allocator really is: measured 2026-09-16 on this device, 56.9 kB "free" with a
    // 47.9 kB "largest block" while byte-addressable DRAM was 7.0 kB with a 4.2 kB
    // ceiling (93% fragmented). That gap is why an OTA-time malloc(1436) could fail.
    // Publish the numbers that actually constrain allocation. heap_def is included to
    // settle what esp_get_free_heap_size() reports - CentralizedFailsafe's low-memory
    // recovery gate uses it against a 20 kB threshold; drop it once that is known.
    doc["heap_dram"] = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    doc["heap_dram_blk"] = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    doc["heap_def"] = esp_get_free_heap_size();
    // Minimum DRAM ever seen since boot. Needed before any change that permanently
    // reserves or reclaims DRAM: current free says nothing about the peak demand, and
    // sizing against a stale figure is exactly how the task-stack trim nearly went
    // wrong (PersistentStorage showed 3096 B spare until get/all ran, then 1580 B).
    doc["heap_dram_min"] = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);

    // Round 16 Issue #5: Add heap fragmentation percentage
    // Fragmentation = 100 - (max_block * 100 / free_heap)
    // Lower is better: 0% = no fragmentation, 100% = completely fragmented
    // Use 64-bit arithmetic to prevent overflow on large heap values
    uint8_t fragPct = (heapFree > 0) ?
        (100 - static_cast<uint8_t>((static_cast<uint64_t>(heapMaxBlock) * 100ULL) / heapFree)) : 100;
    doc["heap_frag"] = fragPct;

    doc["uptime"] = millis() / 1000;

    // Add health monitor data
    // Add basic health data - HealthMonitor not available in this version
    doc["health"]["tasks"] = uxTaskGetNumberOfTasks();
    // stack_min: the minimum free stack across the tasks WE control, from MonitoringTask's last
    // pass. This replaces `stack_hwm`, which was uxTaskGetStackHighWaterMark(NULL) - the MQTT
    // task's OWN headroom, published under a name that reads like a system minimum. It was
    // wrong by whatever the gap happened to be: on 2026-10-26 it published 1604 while the
    // tightest task of ours (NTPTask, trimmed from 4096 to fund RYN4Proc) had 860.
    //
    // Read from MonitoringTask rather than enumerated here: uxTaskGetSystemState() needs a
    // pvPortMalloc of taskCount * sizeof(TaskStatus_t), about 1.3 kB at 32 tasks, and doing
    // that on the health publish path to report on the memory budget would be self-defeating.
    doc["health"]["stack_min"] = MonitoringTask_getMinTaskStackFree();
    doc["health"]["stack_min_task"] = MonitoringTask_getMinTaskStackName();
    // How many scheduled scans in a row could not run. Omitted when 0 (the normal case), so
    // this costs nothing in the payload budget above until it actually matters - and when it
    // is present, stack_min is that many 60 s periods stale rather than current.
    if (const uint16_t stale = MonitoringTask_getStackScanFailures()) {
        doc["health"]["stack_min_age"] = stale;
    }

    auto buffer = MemoryPools::logBufferPool.allocate();
    if (!buffer) {
        LOG_ERROR(TAG, "Failed to allocate buffer for health data");
        return;
    }

    size_t written = serializeJson(doc, buffer->data, sizeof(buffer->data));

    // Backstop for the next field someone adds: warn with headroom left rather than
    // discovering the limit by the topic going silent. Hourly, so it cannot flood.
    constexpr size_t HEALTH_HEADROOM_WARN = 24;
    if (written > 0 && written + HEALTH_HEADROOM_WARN >= sizeof(buffer->data)) {
        static uint32_t lastTightWarnMs = 0;
        const uint32_t nowMs = millis();
        if (lastTightWarnMs == 0 || (nowMs - lastTightWarnMs) >= 3600000UL) {
            lastTightWarnMs = nowMs;
            LOG_WARN(TAG, "Health payload %u B of %u - once it no longer fits, the whole "
                          "publish is DROPPED, not truncated",
                     static_cast<unsigned>(written),
                     static_cast<unsigned>(sizeof(buffer->data)));
        }
    }

    if (written == 0 || written >= sizeof(buffer->data)) {
        LOG_ERROR(TAG, "JSON serialization failed or truncated for health data");
        MemoryPools::logBufferPool.deallocate(buffer);
        return;
    }

    // Queue for publishing with MEDIUM priority
    MQTTTask::publish(MQTT_STATUS_HEALTH, buffer->data, 0, false, MQTTPriority::PRIORITY_MEDIUM);

    // Return buffer to pool
    MemoryPools::logBufferPool.deallocate(buffer);
}

void publishSensorData() {
    auto mqttManager = SRP::getMQTTManager();
    if (!mqttManager || !mqttManager->isConnected()) {
        return;
    }

    // Get sensor data with timeout to avoid blocking
    SemaphoreGuard guard(SRP::getSensorReadingsMutex(), pdMS_TO_TICKS(100));
    if (!guard.hasLock()) {
        LOG_ERROR(TAG, "Failed to acquire sensor mutex for MQTT publish");
        return;
    }

    // Create local copy of sensor data
    SharedSensorReadings sensors = SRP::getSensorReadings();

    // SemaphoreGuard releases automatically when it goes out of scope

    // Create JSON message
    JsonDocument doc;  // ArduinoJson v7

    // Temperature values are in tenths of degrees Celsius (int16_t)
    // Example: 273 = 27.3°C, -50 = -5.0°C

    // Compact format: Use shorter keys for smaller payload
    JsonObject temps = doc["t"].to<JsonObject>();  // temperatures

    // EVERY temperature key is present only while its reading is current. An ABSENT key
    // means "no reading" - that was already the contract for `i` and `p`, and since
    // 2026-10-23 it is the contract for all of them.
    //
    // Why: `bo`, `br`, `wt` and `o` used to be published unconditionally, so whatever sat in
    // the struct went out as if it were a measurement. Home Assistant's long-term statistics
    // still carry the result - all four channels recorded a minimum of -3276.8 °C across two
    // hours on 2025-12-06/07, with hourly MEANS of -1504 and -528 °C. That is TEMP_INVALID
    // (INT16_MIN tenths) divided by ten. No subscriber can defend itself against it either,
    // because the key is present and numeric; the receiving end cannot tell -3276.8 from a
    // reading. Nothing writes TEMP_INVALID into those fields today, which only changes the
    // failure from impossible to invisible: a failed channel published its last good value
    // as though it were current.
    //
    // The flags were already sitting in the struct next to each field, unused here.
    const auto putTemp = [&temps](const char* key, Temperature_t value, bool valid) {
        if (valid && tempIsValid(value)) {
            temps[key] = value;
        }
    };

    putTemp("bo", sensors.boilerTempOutput, sensors.isBoilerTempOutputValid);
    putTemp("br", sensors.boilerTempReturn, sensors.isBoilerTempReturnValid);
    putTemp("wt", sensors.waterHeaterTempTank, sensors.isWaterHeaterTempTankValid);
    putTemp("o", sensors.outsideTemp, sensors.isOutsideTempValid);

    // Optional sensors (enable via ENABLE_SENSOR_* flags)
#ifdef ENABLE_SENSOR_WATER_TANK_TOP
    putTemp("wtt", sensors.waterTankTopTemp, sensors.isWaterTankTopTempValid);
#endif
#ifdef ENABLE_SENSOR_WATER_RETURN
    putTemp("wr", sensors.waterHeaterTempReturn, sensors.isWaterHeaterTempReturnValid);
#endif
#ifdef ENABLE_SENSOR_HEATING_RETURN
    putTemp("hr", sensors.heatingTempReturn, sensors.isHeatingTempReturnValid);
#endif

    // Inside: was already guarded, but on the marker alone - and the struct starts at 0,
    // which tempIsValid() accepts, so the flag belongs here too.
    putTemp("i", sensors.insideTemp, sensors.isInsideTempValid);

    // Add burner target temperature (from BurnerStateMachine)
    bool burnerDemand = false;
    Temperature_t burnerTarget = 0;
    if (BurnerStateMachine::getHeatDemandState(burnerDemand, burnerTarget)) {
        temps["bt"] = burnerTarget;             // burner target
    }

    // Include system pressure if valid (in hundredths of BAR for precision)
    if (sensors.isSystemPressureValid) {
        doc["p"] = sensors.systemPressure;      // pressure in hundredths of BAR
    }

    // Get relay status
    if (xSemaphoreTake(SRP::getRelayReadingsMutex(), pdMS_TO_TICKS(50)) == pdTRUE) {
        SharedRelayReadings relays = SRP::getRelayReadings();
        xSemaphoreGive(SRP::getRelayReadingsMutex());

        // Compact format: combine relay states into a single byte
        // Bit 0: burner, 1: heating_pump, 2: water_pump, 3: half_power, 4: water_mode
        uint8_t relayBits = 0;
        if (relays.relayBurnerEnable) relayBits |= 0x01;
        if (relays.relayHeatingPump) relayBits |= 0x02;
        if (relays.relayWaterPump) relayBits |= 0x04;
        if (relays.relayPowerBoost) relayBits |= 0x08;
        if (relays.relayWaterMode) relayBits |= 0x10;

        doc["r"] = relayBits;  // relays as single byte
    }

    // Add system state as compact byte (like relays)
    // s = system state: bit0=system_enabled, bit1=heating_enabled, bit2=heating_on,
    //                   bit3=water_enabled, bit4=water_on, bit5=water_priority
    EventBits_t systemState = SRP::getSystemStateEventBits();
    uint8_t stateBits = 0;
    if (systemState & SystemEvents::SystemState::BOILER_ENABLED) stateBits |= 0x01;
    if (systemState & SystemEvents::SystemState::HEATING_ENABLED) stateBits |= 0x02;
    if (systemState & SystemEvents::SystemState::HEATING_ON) stateBits |= 0x04;
    if (systemState & SystemEvents::SystemState::WATER_ENABLED) stateBits |= 0x08;
    if (systemState & SystemEvents::SystemState::WATER_ON) stateBits |= 0x10;
    if (systemState & SystemEvents::SystemState::WATER_PRIORITY) stateBits |= 0x20;
    doc["s"] = stateBits;

    // Add sensor fallback status for degraded mode notification
    // sf = sensor fallback: 0=STARTUP (waiting), 1=NORMAL (OK), 2=SHUTDOWN (degraded)
    auto fallbackMode = TemperatureSensorFallback::getCurrentMode();
    doc["sf"] = static_cast<uint8_t>(fallbackMode);

    // If in degraded mode, add which sensors are missing
    if (fallbackMode != TemperatureSensorFallback::FallbackMode::NORMAL) {
        const auto& status = TemperatureSensorFallback::getStatus();
        uint8_t missingSensors = 0;
        if (status.missingBoilerOutput) missingSensors |= 0x01;
        if (status.missingBoilerReturn) missingSensors |= 0x02;
        if (status.missingWaterTemp) missingSensors |= 0x04;
        if (status.missingRoomTemp) missingSensors |= 0x08;
        doc["sm"] = missingSensors;  // sensor missing bits
    }

    // Use the larger JSON buffer pool for sensor data
    auto buffer = MemoryPools::jsonBufferPool.allocate();
    if (!buffer) {
        LOG_ERROR(TAG, "Failed to allocate buffer for sensor data");
        return;
    }

    size_t written = serializeJson(doc, buffer->data, sizeof(buffer->data));
    if (written == 0 || written >= sizeof(buffer->data)) {
        LOG_ERROR(TAG, "JSON serialization failed or truncated (wrote %zu/%zu bytes)",
                 written, sizeof(buffer->data));
        MemoryPools::jsonBufferPool.deallocate(buffer);
        return;
    }

    // Queue for publishing with HIGH priority
    MQTTTask::publish(MQTT_STATUS_SENSORS, buffer->data, 0, false, MQTTPriority::PRIORITY_HIGH);

    // Return buffer to pool
    MemoryPools::jsonBufferPool.deallocate(buffer);
}

void publishSystemState() {
    // System state is now included in sensor data as compact byte "s"
    // s = bit0:system_enabled, bit1:heating_enabled, bit2:heating_on,
    //     bit3:water_enabled, bit4:water_on, bit5:water_priority
    // Individual state topics are published by handleControlCommand() when state changes
    // This function is kept for compatibility but does nothing - state is in sensor data
    LOG_DEBUG(TAG, "System state included in sensor data 's' field");
}

} // namespace MQTTPublisher
