#!/usr/bin/env python3
"""
Send WiFi credentials to buddy ESP32 via USB Serial.
Saves to NVS — survives reboots, no filesystem upload needed.

Usage:
  python3 wifi_setup.py                          # interactive prompt
  python3 wifi_setup.py "MySSID" "MyPassword"    # direct
  python3 wifi_setup.py "MySSID" "MyPass" /dev/ttyACM1  # custom port
"""

import json
import sys
import time

try:
    import serial
except ImportError:
    print("Need pyserial: pip install pyserial")
    print("  or: uv run --with pyserial wifi_setup.py ...")
    sys.exit(1)


def main():
    if len(sys.argv) >= 3:
        ssid = sys.argv[1]
        password = sys.argv[2]
        port = sys.argv[3] if len(sys.argv) > 3 else "/dev/ttyACM0"
    else:
        ssid = input("WiFi SSID: ").strip()
        password = input("WiFi Password: ").strip()
        port = input("Serial port [/dev/ttyACM0]: ").strip() or "/dev/ttyACM0"

    cmd = json.dumps({"wifi_set": {"ssid": ssid, "pass": password}})
    print(f"Sending to {port}: {json.dumps({'wifi_set': {'ssid': ssid, 'pass': '***'}})}")

    ser = serial.Serial(port, 115200, timeout=2)
    time.sleep(0.5)

    ser.write((cmd + "\n").encode())
    time.sleep(1)

    while ser.in_waiting:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if line:
            print(f"  ESP32: {line}")

    ser.close()
    print("Done! ESP32 will connect to WiFi on next boot (or immediately).")
    print(f"Then run: python3 wifi_bridge.py")


if __name__ == "__main__":
    main()
