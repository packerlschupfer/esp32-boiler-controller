/**
 * @file test_scheduler_command_policy.cpp
 * @brief Unit tests for SchedulerCommandPolicy (scheduler enable command, mode defaults, status reply)
 */

#include <unity.h>
#include <cstdint>
#include <cstring>

#include "../../src/modules/scheduler/SchedulerCommandPolicy.h"

// setUp and tearDown are defined in test_main.cpp

using namespace SchedulerCommandPolicy;

namespace {
EnablePayload validEnable() {
    EnablePayload p;
    p.hasId = true;
    p.idIsInt = true;
    p.id = 3;
    p.hasEnabled = true;
    p.enabledIsBool = true;
    return p;
}

bool endsWith(const char* s, const char* suffix) {
    size_t ls = strlen(s);
    size_t lx = strlen(suffix);
    return ls >= lx && strcmp(s + ls - lx, suffix) == 0;
}
}  // namespace

void test_scheduler_enable_payload_validation() {
    TEST_ASSERT_NULL(validateEnable(validEnable()));

    EnablePayload p = validEnable();
    p.hasId = false;
    TEST_ASSERT_EQUAL_STRING("missing_id", validateEnable(p));

    p = validEnable();
    p.idIsInt = false;
    TEST_ASSERT_EQUAL_STRING("invalid_id_type", validateEnable(p));

    p = validEnable();
    p.id = 256;
    TEST_ASSERT_EQUAL_STRING("id_out_of_range", validateEnable(p));
    p.id = -1;
    TEST_ASSERT_EQUAL_STRING("id_out_of_range", validateEnable(p));

    p = validEnable();
    p.hasEnabled = false;
    TEST_ASSERT_EQUAL_STRING("missing_enabled", validateEnable(p));

    p = validEnable();
    p.enabledIsBool = false;  // e.g. "enabled":1 or "enabled":"true"
    TEST_ASSERT_EQUAL_STRING("invalid_enabled_type", validateEnable(p));
}

void test_scheduler_enable_ends_active_run_only_when_disabled() {
    EnableDecision d = decideEnable(true, false, true);
    TEST_ASSERT_TRUE(d.changed);
    TEST_ASSERT_TRUE(d.endActiveRun);

    d = decideEnable(true, false, false);
    TEST_ASSERT_TRUE(d.changed);
    TEST_ASSERT_FALSE(d.endActiveRun);

    d = decideEnable(false, true, false);
    TEST_ASSERT_TRUE(d.changed);
    TEST_ASSERT_FALSE(d.endActiveRun);

    d = decideEnable(true, true, true);  // repeated enable: nothing to save or end
    TEST_ASSERT_FALSE(d.changed);
    TEST_ASSERT_FALSE(d.endActiveRun);
}

void test_scheduler_space_mode_default_targets() {
    TEST_ASSERT_EQUAL_INT(21, spaceDefaultTargetC(SPACE_MODE_COMFORT));
    TEST_ASSERT_EQUAL_INT(18, spaceDefaultTargetC(SPACE_MODE_ECO));
    TEST_ASSERT_EQUAL_INT(10, spaceDefaultTargetC(SPACE_MODE_FROST));

    // Defaults must pass the add handler's space target range check
    using namespace SystemConstants::Temperature::SpaceHeating;
    for (uint8_t mode = SPACE_MODE_COMFORT; mode <= SPACE_MODE_FROST; mode++) {
        TEST_ASSERT_TRUE(spaceDefaultTargetC(mode) >= MIN_TARGET_TEMP / 10);
        TEST_ASSERT_TRUE(spaceDefaultTargetC(mode) <= MAX_TARGET_TEMP / 10);
    }

    TEST_ASSERT_TRUE(spaceModeValid(0));
    TEST_ASSERT_TRUE(spaceModeValid(2));
    TEST_ASSERT_FALSE(spaceModeValid(-1));
    TEST_ASSERT_FALSE(spaceModeValid(3));
}

void test_scheduler_status_lists_active_and_disabled_ids() {
    char out[320];
    const uint8_t active[] = {1};
    const uint8_t disabled[] = {2, 3};
    TEST_ASSERT_EQUAL_STRING("{\"active\":true,\"count\":3,\"activeIds\":[1],\"disabledIds\":[2,3]}",
                             formatStatus(out, sizeof(out), true, 3, active, 1, disabled, 2));
    TEST_ASSERT_EQUAL_STRING("{\"active\":false,\"count\":0,\"activeIds\":[],\"disabledIds\":[]}",
                             formatStatus(out, sizeof(out), false, 0, nullptr, 0, nullptr, 0));
}

void test_scheduler_status_worst_case_fits_mqtt_payload() {
    uint8_t ids[MAX_STATUS_IDS];
    for (size_t i = 0; i < MAX_STATUS_IDS; i++) ids[i] = static_cast<uint8_t>(236 + i);  // 3-digit IDs

    char out[320];  // MQTTPublishRequest::payload
    const char* json = formatStatus(out, sizeof(out), true, 20, ids, MAX_STATUS_IDS, ids, MAX_STATUS_IDS);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_TRUE(strlen(json) < sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"activeIds\":[236,"));
    TEST_ASSERT_NOT_NULL(strstr(json, ",255],\"disabledIds\":[236,"));
    TEST_ASSERT_TRUE(endsWith(json, ",255]}"));
}

void test_scheduler_status_small_buffer_stays_valid_json() {
    uint8_t ids[MAX_STATUS_IDS];
    for (size_t i = 0; i < MAX_STATUS_IDS; i++) ids[i] = static_cast<uint8_t>(236 + i);

    char out[STATUS_MIN_BUFFER];
    const char* json = formatStatus(out, sizeof(out), true, 20, ids, MAX_STATUS_IDS, ids, MAX_STATUS_IDS);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_TRUE(strlen(json) < sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(json, "],\"disabledIds\":["));
    TEST_ASSERT_TRUE(endsWith(json, "]}"));
    TEST_ASSERT_NULL(strstr(json, ",]"));  // no dangling separator

    char tooSmall[STATUS_MIN_BUFFER - 1];
    TEST_ASSERT_NULL(formatStatus(tooSmall, sizeof(tooSmall), true, 20, ids, 1, ids, 1));
}
