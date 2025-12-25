/**
 * @file IRelayController.h
 * @brief Interface for relay control
 */

#ifndef I_RELAY_CONTROLLER_H
#define I_RELAY_CONTROLLER_H

class IRelayController {
public:
    virtual ~IRelayController() = default;
    virtual bool setRelay(int relayNum, bool state) = 0;
    virtual bool getRelay(int relayNum) const = 0;
};

#endif // I_RELAY_CONTROLLER_H