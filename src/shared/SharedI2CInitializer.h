#pragma once
#include <Wire.h>  

// I2C pins configured to avoid Ethernet PHY conflicts
#define I2C_SDA_PIN 33
#define I2C_SCL_PIN 32

class SharedI2CInitializer {
public:
    static void ensureI2CInitialized() {
        if (!i2cInitialized) {
            Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
            i2cInitialized = true;
        }
    }
private:
    static bool i2cInitialized;
};