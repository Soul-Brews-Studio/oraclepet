# ota-base: any firmware over Bluetooth

A small ESP-IDF firmware for any ESP32-S3 with 8 MB of flash. It does one thing: wait for an
app image over Bluetooth LE, install it, boot it.

1. Install it once over USB from the [web flasher](https://soul-brews-studio.github.io/oraclepet/)
   ("OTA base"). The USB install also writes the pet's 3 MB character storage.
2. It advertises as `Claude-XXXX` with the same Nordic UART and Pet OTA services as the pet
   ([`PROTOCOL.md`](../oraclepet-lib/PROTOCOL.md) 4.4), so the
   [Bluetooth tool](https://soul-brews-studio.github.io/oraclepet/ble/), the OraclePet Mac app
   and the Python library can all send it firmware.
3. It checks that the image is an ESP32-S3 app image that fits the slot, checks the CRC-32 of
   the transfer and `esp_ota_end()`'s own image check, writes it to `ota_0` or `ota_1` and
   boots it.

## Layout (8 MB)

| partition | offset | size | |
|---|---|---|---|
| factory | `0x10000` | 960 KB | the base; OTA never writes here |
| ota_0, ota_1 | `0x100000`, `0x300000` | 2 MB each | whatever you send |
| storage | `0x500000` | 3 MB | LittleFS, the pet's characters (mounted by label) |

## Getting back to the base

The base is never overwritten. If the firmware you sent cannot take Bluetooth updates itself,
erase the OTA state (`otadata`, 8 KB at `0xd000`) and the board boots the base again: the web
flasher's **Boot the base again** button does exactly that, or
`esptool.py --chip esp32s3 erase_region 0xd000 0x2000`.

## Limits

- No pairing: anyone in Bluetooth range can send an image while the base advertises.
- No rollback (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=n`), because most firmware does not
  mark itself valid. An image that crashes keeps crashing until you go back to the base.
- The firmware you send runs with this partition table. Pet firmware is fine; firmware that
  expects its own table (other partition names or sizes) may not be.

## Build

```sh
idf.py set-target esp32s3
idf.py build        # ESP-IDF v6.0.3
```
