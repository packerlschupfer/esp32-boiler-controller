# OTA (Over-The-Air) Update Guide

## Overview
The ESPlan Boiler Controller supports Over-The-Air firmware updates via Ethernet connection (ArduinoOTA / espota). This allows remote firmware updates without physical access to the device.

## Prerequisites

1. **Network Connection**: Device must be connected via Ethernet
2. **Known IP Address**: Device IP address (static or via DHCP reservation recommended)
3. **OTA Password**: Set with `-DOTA_PASSWORD` in `credentials.ini`; without it the `ProjectConfig.h` default `update-password` is used
4. **PlatformIO**: Installed on your computer
5. **Python 3**: Required for espota.py tool

## Configuration

### Device Configuration
```cpp
// src/config/ProjectConfig.h (defaults, used only if not set by build flags)
#define OTA_PASSWORD "update-password"  // Overridden by -DOTA_PASSWORD in credentials.ini
#define OTA_PORT 3232                   // Also set by -D OTA_PORT=3232 in platformio.ini
```

### PlatformIO Configuration
Three OTA environments are available (`upload_protocol = espota`):
- `esp32dev_ota_release` - Production firmware (smallest size)
- `esp32dev_ota_debug_selective` - Selective debug logging
- `esp32dev_ota_debug_full` - Full debug logging (largest)

Each has `upload_port = 192.168.20.40` (device IP, `ETH_STATIC_IP` default in `src/config/ProjectConfig.h` and platformio.ini `[base_prod]`) and `upload_flags = --host_ip=192.168.20.16 --auth=${credentials.ota_password}`. Adjust `--host_ip` to your computer. The upload password comes from `ota_password` in `credentials.ini` and must equal `-DOTA_PASSWORD` there (the password built into the running image).

## Update Methods

### Method 1: Using PlatformIO (Recommended)

```bash
pio run -e esp32dev_ota_release -t upload

# Other device IP
pio run -e esp32dev_ota_release -t upload --upload-port 192.168.20.40
```

### Method 2: Manual with espota.py

```bash
# Build firmware
pio run -e esp32dev_ota_release

# Upload using espota.py
python3 ~/.platformio/packages/framework-arduinoespressif32/tools/espota.py \
  -i 192.168.20.40 \
  -p 3232 \
  -a YOUR_OTA_PASSWORD \
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

### Memory Monitoring
Free heap is published with the system health status on `boiler/status/health` (not retained):
```bash
# broker = MQTT_SERVER: 192.168.20.27 (src/config/ProjectConfig.h, platformio.ini [base_prod])
mosquitto_sub -h 192.168.20.27 -u YOUR_MQTT_USER -P YOUR_MQTT_PASSWORD \
  -t "boiler/status/health" -v
```
```json
{"timestamp":964058,"heap_free":60912,"heap_min":58188,"heap_max_blk":49140,"heap_frag":20,"uptime":964,"health":{"tasks":32,"stack_hwm":1084}}
```

There are no `diagnostics/...` MQTT topics: `MQTTDiagnostics` is never initialized.

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

## Testing OTA Updates

### Manual Testing Checklist
- [ ] Verify device IP and connectivity
- [ ] Check current firmware version (`boiler/status/device/firmware`)
- [ ] Monitor free heap (`heap_free` on `boiler/status/health`)
- [ ] Perform update
- [ ] Verify device reboots (`boiler/status/online`)
- [ ] Check new firmware version
- [ ] Test all critical functions

## Troubleshooting

### Common Issues

#### "Connecting to device... FAIL"
- Check device IP address
- Verify device is on same network
- Ensure no firewall blocking port 3232
- Check `--host_ip` in the env's `upload_flags` is your computer's IP

#### "Authentication Failed"
- Verify `ota_password` (used by the `--auth` upload flag) or espota `-a` matches `-DOTA_PASSWORD` in `credentials.ini`, and that the device runs an image built with that password (a changed password takes effect only after the next USB flash)
- An image built without `credentials.build_flags` uses the `ProjectConfig.h` default
- Rebuild and upload via USB if needed

#### "Not Enough Space"
- Device needs ~100KB free heap
- Check `heap_free` / `heap_max_blk` on `boiler/status/health`
- Consider using smaller build (release)

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
- Monitor update success/failure

### 4. Rollback Strategy
- Know how to force rollback
- Keep previous firmware files
- Document rollback procedures
- Test rollback in development

## Security Considerations

### 1. Password Protection
```ini
; credentials.ini (not committed)
[credentials]
build_flags =
    -DOTA_PASSWORD=\"your-secure-password\"
ota_password = your-secure-password   ; upload auth, must be the same value
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
  run: pio run -e esp32dev_ota_release -t upload --upload-port ${{ secrets.DEVICE_IP }}
```

### MQTT and OTA
OTA is not controlled or reported over MQTT:
- There is no MQTT command to start an update; updates are pushed with espota.
- No OTA status or progress topic is published. `OTATask_MQTT.cpp` contains MQTT callbacks (topic `state/ota`) but `OTATask::initWithMQTT()` is never called; `OTATask.cpp` registers the callbacks without MQTT.

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

### Post-Update Verification:
- Check firmware version (`boiler/status/device/firmware`)
- Verify all sensors reading (`boiler/status/sensors`)
- Confirm MQTT connection (`boiler/status/online`)
- Test control functions
