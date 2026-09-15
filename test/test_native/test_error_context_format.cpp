/**
 * @file test_error_context_format.cpp
 * @brief Unit tests for ErrorContextFormat (compact boiler/error/context JSON)
 */

#include <unity.h>
#include <cstring>

#include "../../include/utils/ErrorContextFormat.h"

// setUp and tearDown are defined in test_main.cpp

namespace {
constexpr size_t MQTT_PAYLOAD_SIZE = 320;  // MQTTPublishRequest::payload

ErrorContextFormat::Fields worstCase() {
    ErrorContextFormat::Fields f = {};
    f.errorCode = 2147483647;
    f.component = "ComponentName15";  // ErrorContextSnapshot::component[16]
    f.description = "Description that uses all sixty three characters of the field!!";  // [64]
    f.timestampMs = 4294967295u;
    f.taskName = "TaskNameIs15chr";   // taskName[16]
    f.taskPriority = 25;
    f.freeHeap = 4294967295u;
    f.minFreeHeap = 4294967295u;
    f.largestFreeBlock = 4294967295u;
    f.systemStateBits = 4294967295u;
    f.burnerRequestBits = 4294967295u;
    f.sensorsValid = true;
    f.boilerOutput = -32768;
    f.boilerReturn = -32768;
    f.waterTank = -32768;
    f.pressure = -32768;
    f.relayDesired = 255;
    f.relayActual = 255;
    return f;
}
}  // namespace

void test_error_context_worst_case_fits_mqtt_payload() {
    // 2026-09-15: the old context JSON was ~400 chars and never fitted the 320-byte payload
    char out[MQTT_PAYLOAD_SIZE];
    const ErrorContextFormat::Fields f = worstCase();
    const size_t len = ErrorContextFormat::format(out, sizeof(out), f);
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_TRUE(len < sizeof(out));
    TEST_ASSERT_EQUAL_size_t(len, strlen(out));
    TEST_ASSERT_EQUAL_CHAR('{', out[0]);
    TEST_ASSERT_EQUAL_CHAR('}', out[len - 1]);
    TEST_ASSERT_NOT_NULL(strstr(out, "\"ra\":255}"));
}

void test_error_context_escapes_and_null_sensors() {
    char out[MQTT_PAYLOAD_SIZE];
    ErrorContextFormat::Fields f = worstCase();
    f.description = "say \"hi\"\\\n";
    f.sensorsValid = false;
    ErrorContextFormat::format(out, sizeof(out), f);
    TEST_ASSERT_NOT_NULL(strstr(out, "\"d\":\"say \\\"hi\\\"\\\\\""));  // control char dropped
    TEST_ASSERT_NOT_NULL(strstr(out, "\"bo\":null,\"br\":null,\"wt\":null,\"p\":null"));
}

void test_error_context_small_buffer_shortens_description() {
    char full[MQTT_PAYLOAD_SIZE];
    const ErrorContextFormat::Fields f = worstCase();
    const size_t fullLen = ErrorContextFormat::format(full, sizeof(full), f);
    TEST_ASSERT_TRUE(fullLen > 30);

    char out[MQTT_PAYLOAD_SIZE];
    const size_t smallSize = fullLen - 10;  // too small for the 63-char description
    const size_t len = ErrorContextFormat::format(out, smallSize, f);
    TEST_ASSERT_TRUE(len > 0 && len < smallSize);
    TEST_ASSERT_EQUAL_CHAR('}', out[len - 1]);
    TEST_ASSERT_NULL(strstr(out, "sixty three characters"));  // description was shortened
    char tiny[16];
    TEST_ASSERT_EQUAL_size_t(0, ErrorContextFormat::format(tiny, sizeof(tiny), f));
    TEST_ASSERT_EQUAL_CHAR('\0', tiny[0]);
}
