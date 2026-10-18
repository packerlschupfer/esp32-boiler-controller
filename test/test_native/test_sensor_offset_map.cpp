// test/test_native/test_sensor_offset_map.cpp
#include <unity.h>

#include "modules/tasks/SensorOffsetMap.h"

// Distinct, recognisable values so a mis-mapped field is identifiable by its value
static SensorOffsetMap::Offsets distinctOffsets() {
    SensorOffsetMap::Offsets o;
    o.boilerOutput  = 11;
    o.boilerReturn  = 22;
    o.waterTank     = 33;
    o.outside       = 44;
    o.waterReturn   = 66;
    o.heatingReturn = 77;
    return o;
}

void test_sensor_offset_map_places_each_field_on_its_channel() {
    const auto a = SensorOffsetMap::build(distinctOffsets());

    TEST_ASSERT_EQUAL_INT16(11, a[0]);  // BOILER_OUTPUT
    TEST_ASSERT_EQUAL_INT16(22, a[1]);  // BOILER_RETURN
    TEST_ASSERT_EQUAL_INT16(33, a[2]);  // WATER_TANK
    TEST_ASSERT_EQUAL_INT16(44, a[3]);  // OUTSIDE
    TEST_ASSERT_EQUAL_INT16(66, a[6]);  // WATER_RETURN
    TEST_ASSERT_EQUAL_INT16(77, a[7]);  // HEATING_RETURN
}

// The bug this header exists for: the documented map said outside was CH6.
void test_sensor_offset_map_outside_is_ch3_not_ch6() {
    SensorOffsetMap::Offsets o;
    o.outside = 44;
    const auto a = SensorOffsetMap::build(o);

    TEST_ASSERT_EQUAL_INT16(44, a[SensorIndex::OUTSIDE]);
    TEST_ASSERT_EQUAL_INT16(3, SensorIndex::OUTSIDE);
    TEST_ASSERT_EQUAL_INT16(0, a[6]);  // CH6 is WATER_RETURN, and must stay untouched
}

// Safety property: nothing settable can put a temperature offset on the 4-20 mA
// pressure input. This is the outer of two guards - the MB8ART library also
// refuses a non-temperature channel mode at apply time.
void test_sensor_offset_map_never_offsets_the_pressure_channel() {
    const auto a = SensorOffsetMap::build(distinctOffsets());
    TEST_ASSERT_EQUAL_INT16(0, a[SensorIndex::PRESSURE_CHANNEL]);
    TEST_ASSERT_EQUAL_INT16(4, SensorIndex::PRESSURE_CHANNEL);

    // Also true at the extremes of the registered +-50 range
    SensorOffsetMap::Offsets hi;
    hi.boilerOutput = 50; hi.boilerReturn = 50; hi.waterTank = 50;
    hi.outside = 50; hi.waterReturn = 50; hi.heatingReturn = 50;
    TEST_ASSERT_EQUAL_INT16(0, SensorOffsetMap::build(hi)[SensorIndex::PRESSURE_CHANNEL]);

    SensorOffsetMap::Offsets lo;
    lo.boilerOutput = -50; lo.boilerReturn = -50; lo.waterTank = -50;
    lo.outside = -50; lo.waterReturn = -50; lo.heatingReturn = -50;
    TEST_ASSERT_EQUAL_INT16(0, SensorOffsetMap::build(lo)[SensorIndex::PRESSURE_CHANNEL]);
}

// CH5 has no offset parameter, so it must never receive one either
void test_sensor_offset_map_leaves_tank_top_at_zero() {
    const auto a = SensorOffsetMap::build(distinctOffsets());
    TEST_ASSERT_EQUAL_INT16(0, a[SensorIndex::WATER_TANK_TOP]);
    TEST_ASSERT_EQUAL_INT16(5, SensorIndex::WATER_TANK_TOP);
}

// If two logical offsets shared a channel one would silently win. Setting each
// field alone must light up exactly one slot - which also proves no field is
// dropped on the floor, the other half of a wrong map.
void test_sensor_offset_map_has_no_colliding_or_dropped_fields() {
    const int16_t marker = 25;
    int16_t touched[SensorIndex::MAX_TEMP_SENSORS] = {0};

    struct Field { int16_t SensorOffsetMap::Offsets::* member; };
    const Field fields[] = {
        {&SensorOffsetMap::Offsets::boilerOutput},
        {&SensorOffsetMap::Offsets::boilerReturn},
        {&SensorOffsetMap::Offsets::waterTank},
        {&SensorOffsetMap::Offsets::outside},
        {&SensorOffsetMap::Offsets::waterReturn},
        {&SensorOffsetMap::Offsets::heatingReturn},
    };

    for (const auto& f : fields) {
        SensorOffsetMap::Offsets o;       // all zero
        o.*(f.member) = marker;
        const auto a = SensorOffsetMap::build(o);

        int hits = 0;
        for (uint8_t ch = 0; ch < SensorIndex::MAX_TEMP_SENSORS; ++ch) {
            if (a[ch] == marker) {
                hits++;
                touched[ch]++;
            } else {
                TEST_ASSERT_EQUAL_INT16(0, a[ch]);
            }
        }
        TEST_ASSERT_EQUAL_INT(1, hits);   // exactly one channel, never zero or two
    }

    // Each of the six channels claimed by exactly one field; CH4 and CH5 by none
    TEST_ASSERT_EQUAL_INT(1, touched[0]);
    TEST_ASSERT_EQUAL_INT(1, touched[1]);
    TEST_ASSERT_EQUAL_INT(1, touched[2]);
    TEST_ASSERT_EQUAL_INT(1, touched[3]);
    TEST_ASSERT_EQUAL_INT(0, touched[4]);
    TEST_ASSERT_EQUAL_INT(0, touched[5]);
    TEST_ASSERT_EQUAL_INT(1, touched[6]);
    TEST_ASSERT_EQUAL_INT(1, touched[7]);
}

void test_sensor_offset_map_all_zero_in_all_zero_out() {
    const auto a = SensorOffsetMap::build(SensorOffsetMap::Offsets{});
    for (uint8_t ch = 0; ch < SensorIndex::MAX_TEMP_SENSORS; ++ch) {
        TEST_ASSERT_EQUAL_INT16(0, a[ch]);
    }
}
