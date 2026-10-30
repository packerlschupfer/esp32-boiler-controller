// include/MQTTTopics.h
// Centralized MQTT topic definitions for consistent naming
#ifndef MQTT_TOPICS_H
#define MQTT_TOPICS_H

// Base topic prefix - all topics start with this
#define MQTT_BASE_PREFIX "boiler"

// Status topics (published by device)
#define MQTT_STATUS_PREFIX              MQTT_BASE_PREFIX "/status"
#define MQTT_STATUS_ONLINE              MQTT_STATUS_PREFIX "/online"
#define MQTT_STATUS_DEVICE_IP           MQTT_STATUS_PREFIX "/device/ip"
#define MQTT_STATUS_DEVICE_HOSTNAME     MQTT_STATUS_PREFIX "/device/hostname"
#define MQTT_STATUS_DEVICE_FIRMWARE     MQTT_STATUS_PREFIX "/device/firmware"
#define MQTT_STATUS_DEVICE_BOOT         MQTT_STATUS_PREFIX "/device/boot"
#define MQTT_STATUS_HEALTH              MQTT_STATUS_PREFIX "/health"
#define MQTT_STATUS_SENSORS             MQTT_STATUS_PREFIX "/sensors"
#define MQTT_STATUS_HOTWATER            MQTT_STATUS_PREFIX "/hotwater"

// RS485 bus ownership. Retained, published on every transition: a released bus means the
// controller is deliberately out of service, and that must not be invisible.
#define MQTT_STATUS_BUS                 MQTT_STATUS_PREFIX "/bus"

// Boiler system status topics
#define MQTT_STATUS_SYSTEM              MQTT_STATUS_PREFIX "/system"
#define MQTT_STATUS_HEATING             MQTT_STATUS_PREFIX "/heating"
#define MQTT_STATUS_HEATING_ACTIVE      MQTT_STATUS_PREFIX "/heating/active"
#define MQTT_STATUS_WATER               MQTT_STATUS_PREFIX "/water"
#define MQTT_STATUS_WATER_ACTIVE        MQTT_STATUS_PREFIX "/water/active"
#define MQTT_STATUS_WATER_PRIORITY      MQTT_STATUS_PREFIX "/water/priority"
#define MQTT_STATUS_PID_AUTOTUNE        MQTT_STATUS_PREFIX "/pid/autotune"
#define MQTT_STATUS_PID_PARAMS          MQTT_STATUS_PREFIX "/pid/params"
#define MQTT_STATUS_ERROR               MQTT_STATUS_PREFIX "/error"
#define MQTT_STATUS_BURNER              MQTT_STATUS_PREFIX "/burner"

// FRAM error status topics
#define MQTT_STATUS_FRAM_ERRORS_PREFIX  MQTT_STATUS_PREFIX "/fram_errors"
#define MQTT_STATUS_FRAM_ERRORS_STATS   MQTT_STATUS_FRAM_ERRORS_PREFIX "/stats"
#define MQTT_STATUS_FRAM_ERRORS_ERROR   MQTT_STATUS_FRAM_ERRORS_PREFIX "/error"
#define MQTT_STATUS_FRAM_ERRORS_CLEARED MQTT_STATUS_FRAM_ERRORS_PREFIX "/cleared"
#define MQTT_STATUS_FRAM_ERRORS_LIST    MQTT_STATUS_FRAM_ERRORS_PREFIX "/list"

// FRAM status topics
#define MQTT_STATUS_FRAM_PREFIX         MQTT_STATUS_PREFIX "/fram"
#define MQTT_STATUS_FRAM_INFO           MQTT_STATUS_FRAM_PREFIX "/info"
#define MQTT_STATUS_FRAM_STATUS         MQTT_STATUS_FRAM_PREFIX "/status"
#define MQTT_STATUS_FRAM_ERROR          MQTT_STATUS_FRAM_PREFIX "/error"
#define MQTT_STATUS_FRAM_FORMATTED      MQTT_STATUS_FRAM_PREFIX "/formatted"
#define MQTT_STATUS_FRAM_COUNTERS       MQTT_STATUS_FRAM_PREFIX "/counters"
#define MQTT_STATUS_FRAM_RUNTIME        MQTT_STATUS_FRAM_PREFIX "/runtime"
#define MQTT_STATUS_FRAM_COUNTERS_RESET MQTT_STATUS_FRAM_PREFIX "/counters_reset"
#define MQTT_STATUS_FRAM_PID_SAVE       MQTT_STATUS_FRAM_PREFIX "/pid_save"

// Sensor fallback status topics
#define MQTT_STATUS_SENSOR_FALLBACK     MQTT_STATUS_PREFIX "/sensor_fallback"
#define MQTT_STATUS_SENSOR_MODE         MQTT_STATUS_PREFIX "/sensor_mode"

// Error status topics
#define MQTT_STATUS_ERRORS_PREFIX       MQTT_STATUS_PREFIX "/errors"
#define MQTT_STATUS_ERRORS_LIST         MQTT_STATUS_ERRORS_PREFIX "/list"
#define MQTT_STATUS_ERRORS_ERROR        MQTT_STATUS_ERRORS_PREFIX "/error"
#define MQTT_STATUS_ERRORS_CLEARED      MQTT_STATUS_ERRORS_PREFIX "/cleared"
#define MQTT_STATUS_ERRORS_STATS        MQTT_STATUS_ERRORS_PREFIX "/stats"
#define MQTT_STATUS_ERRORS_CRITICAL     MQTT_STATUS_ERRORS_PREFIX "/critical"
#define MQTT_STATUS_ERRORS_DUMP         MQTT_STATUS_ERRORS_PREFIX "/dump"

// Runtime diagnostics (boiler/cmd/diagnostics). Read-only dumps only - the
// RuntimeDiagnostics enum also carries FORCE_REBOOT, which is deliberately NOT
// reachable from an MQTT payload on a boiler controller
#define MQTT_STATUS_DIAGNOSTICS_PREFIX  MQTT_STATUS_PREFIX "/diagnostics"
#define MQTT_STATUS_DIAGNOSTICS_REPORT  MQTT_STATUS_DIAGNOSTICS_PREFIX "/report"
#define MQTT_STATUS_DIAGNOSTICS_ERROR   MQTT_STATUS_DIAGNOSTICS_PREFIX "/error"

// Command topics (subscribed by device)
#define MQTT_CMD_PREFIX                 MQTT_BASE_PREFIX "/cmd"
#define MQTT_CMD_SYSTEM                 MQTT_CMD_PREFIX "/system"
#define MQTT_CMD_HEATING                MQTT_CMD_PREFIX "/heating"
#define MQTT_CMD_WATER                  MQTT_CMD_PREFIX "/water"
#define MQTT_CMD_WATER_PRIORITY         MQTT_CMD_PREFIX "/water_priority"
#define MQTT_CMD_PID_AUTOTUNE           MQTT_CMD_PREFIX "/pid_autotune"
#define MQTT_CMD_STATUS                 MQTT_CMD_PREFIX "/status"
#define MQTT_CMD_ERRORS                 MQTT_CMD_PREFIX "/errors"
#define MQTT_CMD_FRAM_ERRORS            MQTT_CMD_PREFIX "/fram_errors"
#define MQTT_CMD_FRAM                   MQTT_CMD_PREFIX "/fram"
#define MQTT_CMD_TEST                   MQTT_CMD_PREFIX "/test"
#define MQTT_CMD_DIAGNOSTICS            MQTT_CMD_PREFIX "/diagnostics"

// Scheduler command topics (generic timer scheduler)
// All handled by TimerScheduler::processMQTTCommand via the boiler/cmd/scheduler/+ subscription
#define MQTT_CMD_SCHEDULER_PREFIX       MQTT_CMD_PREFIX "/scheduler"
#define MQTT_CMD_SCHEDULE_ADD           MQTT_CMD_SCHEDULER_PREFIX "/add"
#define MQTT_CMD_SCHEDULE_REMOVE        MQTT_CMD_SCHEDULER_PREFIX "/remove"
#define MQTT_CMD_SCHEDULE_LIST          MQTT_CMD_SCHEDULER_PREFIX "/list"
#define MQTT_CMD_SCHEDULE_STATUS        MQTT_CMD_SCHEDULER_PREFIX "/status"
#define MQTT_CMD_SCHEDULE_ENABLE        MQTT_CMD_SCHEDULER_PREFIX "/enable"
#define MQTT_CMD_SCHEDULE_DISABLE       MQTT_CMD_SCHEDULER_PREFIX "/disable"  // enable with "enabled":false
#define MQTT_CMD_SCHEDULE_CLEAR         MQTT_CMD_SCHEDULER_PREFIX "/clear"    // payload "confirm"
// Removed 2026-09-16: /update, /command, /vacation and /pump_exercise had a topic but no
// handler, so they were accepted and silently ignored. The wildcard subscription still
// delivers them and processMQTTCommand() replies not_implemented (SchedulerCommandPolicy).

// Scheduler status topics
#define MQTT_STATUS_SCHEDULER_PREFIX    MQTT_STATUS_PREFIX "/scheduler"
#define MQTT_STATUS_SCHEDULER_INFO      MQTT_STATUS_SCHEDULER_PREFIX "/info"
#define MQTT_STATUS_SCHEDULER_ACTIVE    MQTT_STATUS_SCHEDULER_PREFIX "/active"
#define MQTT_STATUS_SCHEDULER_NEXT      MQTT_STATUS_SCHEDULER_PREFIX "/next"

// Clock: the RTC's stored instant AND the local wall-clock derived from it. Added 2026-10-25
// with the DST fix, because the RTC was not observable from outside at all - which is how a
// schedule could run an hour off for half the year without anything showing it. Publishing
// both halves makes the conversion checkable: compare `utc` against real UTC and `local`
// against the wall clock, and a stale or mis-derived offset has nowhere to hide.
#define MQTT_STATUS_CLOCK               MQTT_STATUS_PREFIX "/clock"

// Failsafe level, retained, published on change. Added 2026-10-25 with the DEGRADED fix: the
// level was a sticky atomic that nothing published and nothing acted on, so a boiler running
// permanently at half power would have shown no outward sign at all.
#define MQTT_STATUS_FAILSAFE            MQTT_STATUS_PREFIX "/failsafe"

// Scheduler response and event topics
#define MQTT_TOPIC_SCHEDULER_RESPONSE   MQTT_BASE_PREFIX "/scheduler/response"
#define MQTT_TOPIC_SCHEDULER_EVENT      MQTT_BASE_PREFIX "/scheduler/event"
#define MQTT_TOPIC_STATUS               MQTT_BASE_PREFIX "/status"

// Configuration topics
#define MQTT_CONFIG_PREFIX              MQTT_BASE_PREFIX "/config"

// Safety configuration command topics
#define MQTT_CMD_CONFIG_PUMP_PROTECTION  MQTT_CMD_PREFIX "/config/pump_protection_ms"
#define MQTT_CMD_CONFIG_SENSOR_STALE     MQTT_CMD_PREFIX "/config/sensor_stale_ms"
#define MQTT_CMD_CONFIG_POST_PURGE       MQTT_CMD_PREFIX "/config/post_purge_ms"

// Safety configuration status topic
#define MQTT_STATUS_SAFETY_CONFIG        MQTT_STATUS_PREFIX "/safety_config"

// Error context topic (IMPROVEMENT 4)
#define MQTT_ERROR_PREFIX                MQTT_BASE_PREFIX "/error"
#define MQTT_ERROR_CONTEXT               MQTT_ERROR_PREFIX "/context"

// Diagnostics topics (IMPROVEMENT 5)
#define MQTT_DIAGNOSTICS_PREFIX          MQTT_BASE_PREFIX "/diagnostics"
#define MQTT_DIAGNOSTICS_MODBUS_PREFIX   MQTT_DIAGNOSTICS_PREFIX "/modbus"

#endif // MQTT_TOPICS_H