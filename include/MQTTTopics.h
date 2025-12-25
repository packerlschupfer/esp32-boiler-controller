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
#define MQTT_STATUS_HEALTH              MQTT_STATUS_PREFIX "/health"
#define MQTT_STATUS_SENSORS             MQTT_STATUS_PREFIX "/sensors"
#define MQTT_STATUS_HOTWATER            MQTT_STATUS_PREFIX "/hotwater"

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

// Scheduler command topics (generic timer scheduler)
#define MQTT_CMD_SCHEDULER_PREFIX       MQTT_CMD_PREFIX "/scheduler"
#define MQTT_CMD_SCHEDULER_COMMAND      MQTT_CMD_SCHEDULER_PREFIX "/command"
#define MQTT_CMD_SCHEDULE_ADD           MQTT_CMD_SCHEDULER_PREFIX "/add"
#define MQTT_CMD_SCHEDULE_UPDATE        MQTT_CMD_SCHEDULER_PREFIX "/update"
#define MQTT_CMD_SCHEDULE_REMOVE        MQTT_CMD_SCHEDULER_PREFIX "/remove"
#define MQTT_CMD_SCHEDULE_LIST          MQTT_CMD_SCHEDULER_PREFIX "/list"
#define MQTT_CMD_SCHEDULE_ENABLE        MQTT_CMD_SCHEDULER_PREFIX "/enable"
#define MQTT_CMD_SCHEDULE_DISABLE       MQTT_CMD_SCHEDULER_PREFIX "/disable"
#define MQTT_CMD_SCHEDULE_CLEAR         MQTT_CMD_SCHEDULER_PREFIX "/clear"
#define MQTT_CMD_VACATION_SET           MQTT_CMD_SCHEDULER_PREFIX "/vacation"
#define MQTT_CMD_PUMP_EXERCISE          MQTT_CMD_SCHEDULER_PREFIX "/pump_exercise"

// Scheduler status topics
#define MQTT_STATUS_SCHEDULER_PREFIX    MQTT_STATUS_PREFIX "/scheduler"
#define MQTT_STATUS_SCHEDULER_INFO      MQTT_STATUS_SCHEDULER_PREFIX "/info"
#define MQTT_STATUS_SCHEDULER_ACTIVE    MQTT_STATUS_SCHEDULER_PREFIX "/active"
#define MQTT_STATUS_SCHEDULER_NEXT      MQTT_STATUS_SCHEDULER_PREFIX "/next"

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

#endif // MQTT_TOPICS_H