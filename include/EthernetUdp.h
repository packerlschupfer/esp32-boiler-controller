// include/EthernetUdp.h
#ifndef ETHERNET_UDP_H
#define ETHERNET_UDP_H

#include <Arduino.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <ETH.h>

// Minimal EthernetUDP implementation for ESP32 ETH
// Compatible with Arduino Ethernet library API but uses lwIP directly
class EthernetUDP {
private:
    int _sock = -1;
    uint16_t _port = 0;
    IPAddress _remoteIP;
    uint16_t _remotePort = 0;
    uint8_t _buffer[512];  // Buffer for incoming packets
    size_t _bufferSize = 0;
    size_t _bufferPos = 0;
    
public:
    EthernetUDP() {}
    ~EthernetUDP() { stop(); }
    
    // Begin listening on specified port
    uint8_t begin(uint16_t port) {
        // Close any existing socket
        if (_sock >= 0) {
            close(_sock);
        }
        
        // Create UDP socket
        _sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (_sock < 0) {
            return 0;
        }
        
        // Set non-blocking mode
        int flags = fcntl(_sock, F_GETFL, 0);
        fcntl(_sock, F_SETFL, flags | O_NONBLOCK);
        
        // Bind to port
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;
        
        if (bind(_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(_sock);
            _sock = -1;
            return 0;
        }
        
        _port = port;
        return 1;
    }
    
    // Stop listening
    void stop() {
        if (_sock >= 0) {
            close(_sock);
            _sock = -1;
        }
        _port = 0;
    }
    
    // Start a packet to send to the remote host
    int beginPacket(IPAddress ip, uint16_t port) {
        _remoteIP = ip;
        _remotePort = port;
        _bufferSize = 0;
        return 1;
    }
    
    int beginPacket(const char* host, uint16_t port) {
        struct hostent* server = gethostbyname(host);
        if (server == NULL) {
            return 0;
        }
        _remoteIP = IPAddress(server->h_addr_list[0][0], 
                              server->h_addr_list[0][1],
                              server->h_addr_list[0][2], 
                              server->h_addr_list[0][3]);
        _remotePort = port;
        _bufferSize = 0;
        return 1;
    }
    
    // Write data to packet
    size_t write(uint8_t byte) {
        if (_bufferSize < sizeof(_buffer)) {
            _buffer[_bufferSize++] = byte;
            return 1;
        }
        return 0;
    }
    
    size_t write(const uint8_t* buffer, size_t size) {
        size_t written = 0;
        while (written < size && _bufferSize < sizeof(_buffer)) {
            _buffer[_bufferSize++] = buffer[written++];
        }
        return written;
    }
    
    // Send the packet
    int endPacket() {
        if (_sock < 0 || _bufferSize == 0) {
            return 0;
        }
        
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(_remotePort);
        addr.sin_addr.s_addr = _remoteIP;
        
        int sent = sendto(_sock, _buffer, _bufferSize, 0, 
                          (struct sockaddr*)&addr, sizeof(addr));
        _bufferSize = 0;
        
        return (sent > 0) ? 1 : 0;
    }
    
    // Check for incoming packet
    int parsePacket() {
        if (_sock < 0) {
            return 0;
        }
        
        struct sockaddr_in addr;
        socklen_t addrLen = sizeof(addr);
        
        int len = recvfrom(_sock, _buffer, sizeof(_buffer), MSG_DONTWAIT,
                           (struct sockaddr*)&addr, &addrLen);
        
        if (len > 0) {
            _bufferSize = len;
            _bufferPos = 0;
            _remoteIP = IPAddress(addr.sin_addr.s_addr);
            _remotePort = ntohs(addr.sin_port);
            return len;
        }
        
        return 0;
    }
    
    // Number of bytes available to read
    int available() {
        return _bufferSize - _bufferPos;
    }
    
    // Read a single byte
    int read() {
        if (_bufferPos < _bufferSize) {
            return _buffer[_bufferPos++];
        }
        return -1;
    }
    
    // Read data into buffer
    int read(uint8_t* buffer, size_t len) {
        int copied = 0;
        while (copied < len && _bufferPos < _bufferSize) {
            buffer[copied++] = _buffer[_bufferPos++];
        }
        return copied;
    }
    
    // Peek at next byte without consuming
    int peek() {
        if (_bufferPos < _bufferSize) {
            return _buffer[_bufferPos];
        }
        return -1;
    }
    
    // Flush any pending data
    void flush() {
        // No-op for UDP
    }
    
    // Get remote IP of last received packet
    IPAddress remoteIP() {
        return _remoteIP;
    }
    
    // Get remote port of last received packet
    uint16_t remotePort() {
        return _remotePort;
    }
};

#endif // ETHERNET_UDP_H