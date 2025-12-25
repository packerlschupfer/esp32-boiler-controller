#!/usr/bin/env python3
"""
Modbus RTU Sniffer for RS485 Bus
Captures and decodes traffic between ESP32 and ANDRTF3 sensor

Hardware setup:
  - Connect USB-RS485 adapter A/B to the bus
  - DO NOT connect TX (receive-only mode)

Usage:
  ./sniff_modbus.py [device] [logfile] [--clean]

Options:
  --clean    Only show CRC:OK frames (hide noise)
"""

import sys
import serial
import termios
import time
from datetime import datetime

# Colors for terminal
RED = '\033[0;31m'
GREEN = '\033[0;32m'
YELLOW = '\033[1;33m'
BLUE = '\033[0;34m'
CYAN = '\033[0;36m'
NC = '\033[0m'

# Modbus function codes
FUNC_CODES = {
    1: "Read Coils",
    2: "Read Discrete Inputs",
    3: "Read Holding Registers",
    4: "Read Input Registers",
    5: "Write Single Coil",
    6: "Write Single Register",
    15: "Write Multiple Coils",
    16: "Write Multiple Registers",
}

# Exception codes
EXCEPTION_CODES = {
    1: "Illegal Function",
    2: "Illegal Data Address",
    3: "Illegal Data Value",
    4: "Slave Device Failure",
    5: "Acknowledge",
    6: "Slave Device Busy",
    8: "Memory Parity Error",
}


def calc_crc16(data: bytes) -> int:
    """Calculate Modbus CRC16"""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc


def verify_crc(frame: bytes) -> bool:
    """Verify CRC of a Modbus frame"""
    if len(frame) < 4:
        return False
    data = frame[:-2]
    received_crc = frame[-2] | (frame[-1] << 8)
    calculated_crc = calc_crc16(data)
    return received_crc == calculated_crc


def decode_frame(frame: bytes, timestamp: str, logfile, show_bad_crc: bool = True) -> None:
    """Decode and display a Modbus RTU frame"""
    hex_str = frame.hex().upper()

    if len(frame) < 4:
        if show_bad_crc:
            print(f"{YELLOW}[{timestamp}] Incomplete frame ({len(frame)} bytes): {hex_str}{NC}")
        logfile.write(f"[{timestamp}] INCOMPLETE: {hex_str}\n")
        return

    addr = frame[0]
    func = frame[1]
    is_exception = func >= 0x80

    if is_exception:
        func -= 0x80

    func_name = FUNC_CODES.get(func, "Unknown")

    # Verify CRC
    crc_ok = verify_crc(frame)
    crc_str = f"{GREEN}CRC:OK{NC}" if crc_ok else f"{RED}CRC:BAD{NC}"
    crc_log = "OK" if crc_ok else "BAD"

    # Skip bad CRC frames in console if filter enabled
    if not crc_ok and not show_bad_crc:
        logfile.write(f"[{timestamp}] RAW: {hex_str} CRC:BAD\n")
        return

    # Log raw frame
    logfile.write(f"[{timestamp}] RAW: {hex_str}\n")

    if is_exception:
        exc_code = frame[2] if len(frame) > 2 else 0
        exc_name = EXCEPTION_CODES.get(exc_code, "Unknown")
        print(f"{RED}[{timestamp}] EXCEPTION from addr {addr}: {exc_name} (code {exc_code}){NC}")
        logfile.write(f"[{timestamp}] EXCEPTION: addr={addr} func={func} exc={exc_code} ({exc_name})\n")
        return

    if func == 4:  # Read Input Registers
        if len(frame) == 8:
            # Request: addr(1) + func(1) + reg(2) + count(2) + crc(2)
            reg = (frame[2] << 8) | frame[3]
            count = (frame[4] << 8) | frame[5]
            print(f"{CYAN}[{timestamp}] REQUEST: addr={addr} func={func} ({func_name}) reg={reg} count={count} {crc_str}{NC}")
            logfile.write(f"[{timestamp}] REQUEST: addr={addr} func={func} reg={reg} count={count} crc={crc_log}\n")
        else:
            # Response: addr(1) + func(1) + bytecount(1) + data(n) + crc(2)
            bytecount = frame[2]
            data_bytes = frame[3:3+bytecount]

            # Parse register values (16-bit signed)
            values = []
            for i in range(0, len(data_bytes), 2):
                if i + 1 < len(data_bytes):
                    val = (data_bytes[i] << 8) | data_bytes[i+1]
                    if val >= 0x8000:
                        val -= 0x10000
                    values.append(val)

            # Temperature interpretation
            temp_info = ""
            if values:
                first_val = values[0]
                if first_val == 0:
                    temp_info = f" {RED}[0x0000 SENSOR ERROR]{NC}"
                elif first_val == -1:
                    temp_info = f" {RED}[0xFFFF MODBUS ERROR]{NC}"
                elif -400 <= first_val <= 1250:
                    temp_c = first_val / 10.0
                    temp_info = f" {GREEN}[{temp_c:.1f}°C]{NC}"
                else:
                    temp_info = f" {YELLOW}[OUT OF RANGE]{NC}"

            values_str = " ".join(str(v) for v in values)
            print(f"{GREEN}[{timestamp}] RESPONSE: addr={addr} func={func} bytes={bytecount} values={values_str}{temp_info} {crc_str}{NC}")
            logfile.write(f"[{timestamp}] RESPONSE: addr={addr} func={func} bytes={bytecount} values={values_str} crc={crc_log}\n")

    elif func == 3:  # Read Holding Registers
        if len(frame) == 8:
            reg = (frame[2] << 8) | frame[3]
            count = (frame[4] << 8) | frame[5]
            print(f"{CYAN}[{timestamp}] REQUEST: addr={addr} func={func} ({func_name}) reg={reg} count={count} {crc_str}{NC}")
            logfile.write(f"[{timestamp}] REQUEST: addr={addr} func={func} reg={reg} count={count} crc={crc_log}\n")
        else:
            bytecount = frame[2]
            data_hex = frame[3:3+bytecount].hex().upper()
            print(f"{GREEN}[{timestamp}] RESPONSE: addr={addr} func={func} bytes={bytecount} data={data_hex} {crc_str}{NC}")
            logfile.write(f"[{timestamp}] RESPONSE: addr={addr} func={func} bytes={bytecount} data={data_hex} crc={crc_log}\n")

    else:
        # Generic frame
        print(f"{BLUE}[{timestamp}] FRAME: addr={addr} func={func} ({func_name}) len={len(frame)} {crc_str}{NC}")
        logfile.write(f"[{timestamp}] FRAME: addr={addr} func={func} len={len(frame)} hex={hex_str} crc={crc_log}\n")


def main():
    # Parse arguments
    args = [arg for arg in sys.argv[1:] if not arg.startswith('--')]
    flags = [arg for arg in sys.argv[1:] if arg.startswith('--')]

    device = args[0] if len(args) > 0 else "/dev/ttyUSB485"
    logfile_name = args[1] if len(args) > 1 else f"modbus_capture_{datetime.now().strftime('%Y%m%d_%H%M%S')}.log"
    show_bad_crc = '--clean' not in flags

    print(f"{BLUE}======================================{NC}")
    print(f"{BLUE}Modbus RTU Sniffer (Python){NC}")
    print(f"{BLUE}======================================{NC}")
    print(f"Device:  {device}")
    print(f"Logfile: {logfile_name}")
    print(f"Mode:    {'CLEAN (CRC:OK only)' if not show_bad_crc else 'ALL FRAMES'}")
    print()
    print(f"{YELLOW}IMPORTANT: USB-RS485 adapter must be in receive-only mode!{NC}")
    print(f"{YELLOW}           (TX line disconnected or floating){NC}")
    print()
    print("Press Ctrl+C to stop")
    print()

    try:
        # All devices use 8E1 (Even parity) - Modbus RTU standard
        ser = serial.Serial(
            port=device,
            baudrate=9600,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_EVEN,  # 8E1 after parity fix
            stopbits=serial.STOPBITS_ONE,
            timeout=0.001,  # 1ms timeout for low latency
            write_timeout=0,
            inter_byte_timeout=None
        )
        # FTDI RS485 adapters: RTS typically controls DE (Driver Enable)
        # RTS=False (LOW) should disable transmitter
        ser.rts = False
        ser.dtr = False

        # Aggressive Linux serial optimizations
        attrs = termios.tcgetattr(ser.fd)
        # Disable echo
        attrs[3] = attrs[3] & ~termios.ECHO
        # Enable low latency mode (TIOCSSERIAL ioctl equivalent)
        # This reduces the kernel's buffering delays
        termios.tcsetattr(ser.fd, termios.TCSANOW, attrs)

        # Try to set low-latency flag (Linux-specific)
        try:
            import fcntl
            import struct
            # TIOCGSERIAL = 0x541E, TIOCSSERIAL = 0x541F
            TIOCGSERIAL = 0x541E
            TIOCSSERIAL = 0x541F

            # Get current serial settings
            buf = bytearray(60)  # struct serial_struct size
            fcntl.ioctl(ser.fd, TIOCGSERIAL, buf)

            # Set low-latency flag (bit 13 in flags)
            flags_offset = 12  # offset of flags in serial_struct
            flags = struct.unpack('I', buf[flags_offset:flags_offset+4])[0]
            flags |= 0x2000  # ASYNC_LOW_LATENCY
            struct.pack_into('I', buf, flags_offset, flags)

            # Apply settings
            fcntl.ioctl(ser.fd, TIOCSSERIAL, buf)
            print(f"{CYAN}FTDI: RTS=LOW (TX disabled), Low-latency mode enabled{NC}")
        except Exception as e:
            print(f"{CYAN}FTDI: RTS=LOW (TX disabled), Echo disabled{NC}")
            print(f"{YELLOW}(Could not enable kernel low-latency mode: {e}){NC}")
    except serial.SerialException as e:
        print(f"{RED}Error opening {device}: {e}{NC}")
        sys.exit(1)

    # Reduce USB latency timer for FTDI devices
    import os
    import subprocess
    try:
        # Find the device path
        dev_name = os.path.basename(device)
        latency_path = f"/sys/bus/usb-serial/devices/{dev_name}/latency_timer"

        if os.path.exists(latency_path):
            # Read current latency
            with open(latency_path, 'r') as f:
                current_latency = f.read().strip()
            print(f"USB latency: {current_latency}ms (default: 16ms)")

            # Try to set to 1ms (needs root, may fail)
            try:
                subprocess.run(['sudo', 'tee', latency_path],
                             input=b'1', check=True, capture_output=True)
                print(f"{GREEN}USB latency reduced to 1ms{NC}")
            except:
                print(f"{YELLOW}Could not reduce latency (try: sudo echo 1 > {latency_path}){NC}")
    except Exception as e:
        pass  # Not an FTDI device or permission issue

    print(f"{GREEN}Listening...{NC}")
    print()

    # Flush any garbage from buffer
    ser.reset_input_buffer()
    time.sleep(0.1)  # Wait 100ms for bus to settle

    # Frame detection
    # At 9600 baud 8E1: 11 bits per byte = ~1.15ms per byte
    # 3.5 character times = ~4ms inter-frame gap
    # With low-latency optimizations, use 6ms (was 10ms)
    FRAME_GAP_MS = 6  # ms between frames (optimized for low-latency mode)

    frame_buffer = bytearray()
    last_byte_time = 0
    frames_processed = 0

    with open(logfile_name, 'w') as logfile:
        logfile.write(f"# Modbus RTU Capture started {datetime.now()}\n")
        logfile.write(f"# Device: {device}\n")
        logfile.write("\n")
        logfile.flush()

        try:
            while True:
                # Read available bytes
                if ser.in_waiting > 0:
                    data = ser.read(ser.in_waiting)
                    current_time = time.time() * 1000  # ms

                    # Check for frame gap
                    if frame_buffer and last_byte_time > 0:
                        gap = current_time - last_byte_time
                        if gap > FRAME_GAP_MS:
                            # Process previous frame
                            timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
                            decode_frame(bytes(frame_buffer), timestamp, logfile, show_bad_crc)
                            logfile.flush()
                            frame_buffer = bytearray()

                    frame_buffer.extend(data)
                    last_byte_time = current_time

                else:
                    # No data available
                    if frame_buffer:
                        current_time = time.time() * 1000
                        gap = current_time - last_byte_time
                        if gap > FRAME_GAP_MS:
                            # Frame complete
                            timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
                            decode_frame(bytes(frame_buffer), timestamp, logfile, show_bad_crc)
                            logfile.flush()
                            frame_buffer = bytearray()
                            last_byte_time = 0

                    time.sleep(0.001)  # 1ms sleep when idle

        except KeyboardInterrupt:
            print()
            print(f"{BLUE}Capture stopped.{NC}")
            if frame_buffer:
                timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
                decode_frame(bytes(frame_buffer), timestamp, logfile, show_bad_crc)

        finally:
            ser.close()

    print(f"Log saved to: {logfile_name}")


if __name__ == "__main__":
    main()
