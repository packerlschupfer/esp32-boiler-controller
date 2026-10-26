// include/config/PIDControllerIDs.h
#ifndef PID_CONTROLLER_IDS_H
#define PID_CONTROLLER_IDS_H

/**
 * @brief Unique identifiers for PID controllers in the system
 * 
 * These IDs are used to save/restore PID controller states to/from FRAM
 */
enum PIDControllerId : uint8_t {
    PID_CONTROLLER_SPACE_HEATING = 0,   // Space heating PID controller
    PID_CONTROLLER_WATER_HEATING = 1,   // Water heating PID controller
    PID_CONTROLLER_MAX = 2              // Total number of PID controllers
};

#endif // PID_CONTROLLER_IDS_H