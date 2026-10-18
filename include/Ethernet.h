// include/Ethernet.h
#ifndef ETHERNET_H
#define ETHERNET_H

// Minimal Ethernet.h for ESP32 ETH compatibility
// This just includes the ESP32 ETH library and provides the EthernetUDP class

#include <ETH.h>
#include "EthernetUdp.h"

// For compatibility, provide a dummy Ethernet class
// The actual Ethernet functionality is handled by ESP32's ETH library
class EthernetClass {
public:
    // These methods are not needed for UDP-only usage
    void begin(uint8_t* mac) {}
    void begin(uint8_t* mac, IPAddress ip) {}
    int maintain() { return 0; }
    IPAddress localIP() { return ETH.localIP(); }
    IPAddress subnetMask() { return ETH.subnetMask(); }
    IPAddress gatewayIP() { return ETH.gatewayIP(); }
    IPAddress dnsServerIP() { return ETH.dnsIP(); }
};

extern EthernetClass Ethernet;

#endif // ETHERNET_H