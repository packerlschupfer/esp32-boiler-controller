/**
 * @file test_temperature_conversion.cpp
 * @brief Unit tests for Temperature_t conversion functions
 */

#include <unity.h>
#include <cstdio>
#include <cstring>

// Include the Temperature.h header
#include "../../include/shared/Temperature.h"

// setUp and tearDown are defined in test_main.cpp

// Test basic float to Temperature_t conversion
void test_float_to_temperature_conversion() {
    // Test positive temperatures
    TEST_ASSERT_EQUAL_INT16(234, tempFromFloat(23.4f));
    TEST_ASSERT_EQUAL_INT16(250, tempFromFloat(25.0f));
    TEST_ASSERT_EQUAL_INT16(1000, tempFromFloat(100.0f));
    
    // Test negative temperatures
    TEST_ASSERT_EQUAL_INT16(-100, tempFromFloat(-10.0f));
    TEST_ASSERT_EQUAL_INT16(-234, tempFromFloat(-23.4f));
    
    // Test zero
    TEST_ASSERT_EQUAL_INT16(0, tempFromFloat(0.0f));
    
    // Test rounding - 23.45 * 10 = 234.5, which rounds to 234 (not 235)
    TEST_ASSERT_EQUAL_INT16(234, tempFromFloat(23.45f));  // 234.5 rounds down
    TEST_ASSERT_EQUAL_INT16(234, tempFromFloat(23.44f));  // Should round to 23.4
}

// Test Temperature_t to float conversion
void test_temperature_to_float_conversion() {
    // Test positive temperatures
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 23.4f, tempToFloat(234));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 25.0f, tempToFloat(250));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, tempToFloat(1000));
    
    // Test negative temperatures
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -10.0f, tempToFloat(-100));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -23.4f, tempToFloat(-234));
    
    // Test zero
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, tempToFloat(0));
}

// Test temperature addition
void test_temperature_addition() {
    Temperature_t temp1 = tempFromFloat(20.5f);
    Temperature_t temp2 = tempFromFloat(5.3f);
    
    Temperature_t result = tempAdd(temp1, temp2);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 25.8f, tempToFloat(result));
    
    // Test negative addition
    temp1 = tempFromFloat(20.5f);
    temp2 = tempFromFloat(-5.3f);
    result = tempAdd(temp1, temp2);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 15.2f, tempToFloat(result));
}

// Test temperature subtraction
void test_temperature_subtraction() {
    Temperature_t temp1 = tempFromFloat(20.5f);
    Temperature_t temp2 = tempFromFloat(5.3f);
    
    Temperature_t result = tempSub(temp1, temp2);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 15.2f, tempToFloat(result));
    
    // Test negative result
    temp1 = tempFromFloat(5.3f);
    temp2 = tempFromFloat(20.5f);
    result = tempSub(temp1, temp2);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -15.2f, tempToFloat(result));
}

// Test temperature comparison
void test_temperature_comparison() {
    Temperature_t temp1 = tempFromFloat(20.5f);
    Temperature_t temp2 = tempFromFloat(20.5f);
    Temperature_t temp3 = tempFromFloat(25.0f);
    Temperature_t temp4 = tempFromFloat(15.0f);
    
    // Test equality
    TEST_ASSERT_TRUE(temp1 == temp2);
    TEST_ASSERT_FALSE(temp1 == temp3);
    
    // Test greater than
    TEST_ASSERT_TRUE(temp3 > temp1);
    TEST_ASSERT_FALSE(temp1 > temp3);
    TEST_ASSERT_FALSE(temp1 > temp2);
    
    // Test less than
    TEST_ASSERT_TRUE(temp4 < temp1);
    TEST_ASSERT_FALSE(temp1 < temp4);
    TEST_ASSERT_FALSE(temp1 < temp2);
}

// Test temperature formatting
void test_temperature_formatting() {
    char buffer[32];
    
    // Test positive temperature
    formatTemp(buffer, sizeof(buffer), tempFromFloat(23.4f));
    TEST_ASSERT_EQUAL_STRING("23.4", buffer);
    
    // Test negative temperature
    formatTemp(buffer, sizeof(buffer), tempFromFloat(-15.7f));
    TEST_ASSERT_EQUAL_STRING("-15.7", buffer);
    
    // Test zero
    formatTemp(buffer, sizeof(buffer), tempFromFloat(0.0f));
    TEST_ASSERT_EQUAL_STRING("0.0", buffer);
    
    // Test rounding display - 99.99 * 10 = 999.9, which is 999 as int16_t = 99.9°C
    formatTemp(buffer, sizeof(buffer), tempFromFloat(99.99f));
    TEST_ASSERT_EQUAL_STRING("99.9", buffer);
}

// Test invalid temperature detection
void test_invalid_temperature() {
    // Valid range tests - temperatures are valid if within reasonable bounds
    Temperature_t validTemp1 = tempFromFloat(25.0f);
    Temperature_t validTemp2 = tempFromFloat(-30.0f);
    Temperature_t validTemp3 = tempFromFloat(100.0f);
    
    TEST_ASSERT_NOT_EQUAL(TEMP_INVALID, validTemp1);
    TEST_ASSERT_NOT_EQUAL(TEMP_INVALID, validTemp2);
    TEST_ASSERT_NOT_EQUAL(TEMP_INVALID, validTemp3);
    
    // Test known invalid value
    TEST_ASSERT_EQUAL(TEMP_INVALID, INT16_MIN);
}

// Test edge cases
void test_temperature_edge_cases() {
    // Test maximum positive value
    Temperature_t maxTemp = 32767;  // Maximum int16_t
    float maxFloat = tempToFloat(maxTemp);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 3276.7f, maxFloat);
    
    // Test minimum valid negative value (not TEMP_INVALID)
    Temperature_t minTemp = -32767;  // Minimum valid int16_t 
    float minFloat = tempToFloat(minTemp);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, -3276.7f, minFloat);
    
    // Test TEMP_INVALID constant
    TEST_ASSERT_EQUAL_INT16(-32768, TEMP_INVALID);
}

// Test temperature difference calculation
void test_temperature_difference() {
    Temperature_t temp1 = tempFromFloat(25.0f);
    Temperature_t temp2 = tempFromFloat(20.0f);
    
    // Calculate difference manually since tempDiff may not be available
    int16_t diff = temp1 - temp2;
    TEST_ASSERT_EQUAL_INT16(50, diff);  // 5.0°C difference
    
    // Test negative difference
    diff = temp2 - temp1;
    TEST_ASSERT_EQUAL_INT16(-50, diff);  // -5.0°C difference
}

// main() is in test_main.cpp