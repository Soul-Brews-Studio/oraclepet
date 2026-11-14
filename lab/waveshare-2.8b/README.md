# Waveshare ESP32-S3-LCD-2.8B

480x640 RGB parallel display (ST7701S) with LVGL dashboard.

## Board

| Spec | Value |
|------|-------|
| MCU | ESP32-S3 (16MB flash, 8MB PSRAM) |
| Display | 2.8" 480x640 RGB, ST7701S |
| Touch | None (LCD-only variant) |
| I2C Devices | TCA9554 (0x20), PCF8563 RTC (0x51), QMI8658 IMU (0x6B) |
| Buzzer | Passive, via TCA9554 EXIO7 |
| Backlight | GPIO6 (LEDC PWM) |

## Quick Start

```bash
cp secrets.yaml.example secrets.yaml
# edit secrets.yaml with your WiFi credentials
make run
```

First flash: hold BOOT → press RESET → release BOOT, then `make run`.
After first flash: OTA updates via WiFi.

## Dashboard

- Clock (RTC + NTP sync, Asia/Bangkok)
- Name display
- WiFi status
- Uptime counter
- Heartbeat
- Buzzer beep on boot
