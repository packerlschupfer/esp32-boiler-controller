# OTA (Over-The-Air) Update Guide

## Overview
The ESPlan Boiler Controller supports Over-The-Air firmware updates via Ethernet connection. This allows remote firmware updates without physical access to the device.

## Prerequisites

1. **Network Connection**: Device must be connected via Ethernet
2. **Known IP Address**: Device IP address (static or via DHCP reservation recommended)
3. **OTA Password**: Default is `update-password` (change in production!)
4. **PlatformIO**: Installed on your computer
5. **Python 3**: Required for espota.py tool

## Configuration

### Device Configuration (ProjectConfig.h)
```cpp
#define OTA_PASSWORD "update-password"  // Change this!
#define OTA_PORT 3232                   // Default OTA port
```

### PlatformIO Configuration
Three OTA environments are available:
- `esp32dev_ota_release` - Production firmware (smallest size)
- `esp32dev_ota_debug_selective` - Selective debug logging
- `esp32dev_ota_debug_full` - Full debug logging (largest)

## Update Methods

### Method 1: Using ota_update.sh Script (Recommended)

```bash
# Basic usage
./ota_update.sh <device_ip>

# Example
./ota_update.sh 192.168.16.138

# With specific environment
./ota_update.sh 192.168.16.138 esp32dev_ota_debug_selective
```

### Method 2: Using PlatformIO Directly

1. Edit `platformio.ini` to set your device IP:
```ini
[env:esp32dev_ota_release]
upload_port = YOUR_DEVICE_IP_HERE
```

2. Upload firmware:
```bash
pio run -e esp32dev_ota_release -t upload
```

### Method 3: Manual with espota.py

```bash
# Build firmware
pio run -e esp32dev_ota_release

# Upload using espota.py
python3 ~/.platformio/packages/framework-arduinoespressif32/tools/espota.py \
  -i 192.168.16.138 \
  -p 3232 \
  -a update-password \
  -f .pio/build/esp32dev_ota_release/firmware.bin \
  --progress
```

## Memory Considerations

### Firmware Sizes (Approximate)
- Release build: ~1.2 MB
- Debug selective: ~1.3 MB
- Debug full: ~1.4 MB

### Memory Requirements
- Minimum free heap: 100 KB during update
- OTA partition size: 1.5 MB (configured in partitions)
- Update requires 2x firmware size temporarily

### Memory Monitoring During Update
Monitor memory via MQTT:
```bash
mosquitto_sub -h 192.168.16.16 -u YOUR_MQTT_USER -P password \
  -t "cmd/boiler/diagnostics/memory/response" -v
```

## Safety Features

### 1. Dual Partition System
- Current firmware runs from one partition
- New firmware uploads to alternate partition
- Automatic rollback on boot failure

### 2. Pre-Update Checks
- Verify sufficient free memory
- Check network connectivity
- Ensure no critical operations in progress

### 3. Update Process Safety
- Watchdog disabled during update
- Non-critical tasks may be suspended
- MQTT diagnostics continue if possible

## Testing OTA Updates

### Automated Test Suite
```bash
python3 test_ota_update.py
```

This test suite:
1. Discovers device via mDNS
2. Monitors memory before update
3. Builds and uploads firmware
4. Verifies successful update
5. Analyzes memory impact
6. Generates test report

### Manual Testing Checklist
- [ ] Verify device IP and connectivity
- [ ] Check current firmware version
- [ ] Monitor free heap (should be >100KB)
- [ ] Perform update
- [ ] Verify device reboots
- [ ] Check new firmware version
- [ ] Test all critical functions

## Troubleshooting

### Common Issues

#### "Connecting to device... FAIL"
- Check device IP address
- Verify device is on same network
- Ensure no firewall blocking port 3232

#### "Authentication Failed"
- Verify OTA password matches device
- Check password in ProjectConfig.h
- Rebuild and upload via USB if needed

#### "Not Enough Space"
- Device needs ~100KB free heap
- Check memory diagnostics
- Consider using smaller build (release)
- May need to clear MQTT diagnostics

#### Update Succeeds but Device Doesn't Boot
- Automatic rollback should occur
- Device reverts to previous firmware
- Check serial console for boot errors
- Verify firmware compatibility

### Recovery Procedures

#### If OTA Fails Repeatedly:
1. Connect via USB/Serial
2. Upload firmware directly:
   ```bash
   pio run -e esp32dev_usb_release -t upload
   ```
3. Check serial output for errors
4. Verify partition table correct

#### If Device Becomes Unresponsive:
1. Power cycle the device
2. Hold BOOT button during power on (if available)
3. Upload via USB in download mode
4. Check for bootloop issues

## Best Practices

### 1. Pre-Update Preparation
- **Always** test updates in development first
- Monitor device health before updating
- Schedule updates during maintenance windows
- Have physical access plan as backup

### 2. Version Management
- Increment version in ProjectConfig.h
- Tag releases in git
- Keep firmware archive of known-good versions
- Document changes in each version

### 3. Production Deployment
- Change default OTA password
- Use static IP or DHCP reservation
- Implement update scheduling via MQTT
- Monitor update success/failure

### 4. Rollback Strategy
- Know how to force rollback
- Keep previous firmware files
- Document rollback procedures
- Test rollback in development

## Security Considerations

### 1. Password Protection
```cpp
// In credentials.ini or build flags
-DOTA_PASSWORD=\"your-secure-password\"
```

### 2. Network Security
- Use isolated IoT VLAN
- Restrict OTA port access
- Monitor for unauthorized attempts

### 3. Firmware Signing (Future)
- ESP32 supports secure boot
- Implement signed updates
- Verify firmware authenticity

## Automation

### CI/CD Integration
```yaml
# Example GitHub Actions
- name: Build OTA Firmware
  run: pio run -e esp32dev_ota_release
  
- name: Deploy to Device
  run: ./ota_update.sh ${{ secrets.DEVICE_IP }}
  env:
    OTA_PASSWORD: ${{ secrets.OTA_PASSWORD }}
```

### MQTT-Triggered Updates
Future enhancement to trigger OTA via MQTT:
```
Topic: cmd/boiler/ota/start
Payload: {"url": "http://server/firmware.bin", "checksum": "md5hash"}
```

## Performance Impact

### During Update:
- CPU usage: High (80-90%)
- Memory usage: +50-100KB
- Network: ~100KB/s transfer
- Duration: 30-60 seconds
- Services: May be degraded

### After Update:
- Full reboot required
- Services restart time: ~10s
- Memory reinitialization
- Network reconnection

## Monitoring and Logging

### Enable OTA Logging:
```cpp
esp_log_level_set("OTA", ESP_LOG_DEBUG);
```

### MQTT Status Updates:
```json
{
  "status": "updating",
  "progress": 45,
  "total": 1234567,
  "speed": 98304
}
```

### Post-Update Verification:
- Check firmware version
- Verify all sensors reading
- Confirm MQTT connection
- Test control functions