# ATS Mini boot manager (dual-boot)

> **⚠️ Work in progress** — Still under development. Crashes, boot failures,
> data loss, etc. may occur. Use at your own risk.

한국어 문서: [README.md](README.md)

A small boot manager for the ATS Mini (ESP32-S3 + SI4732) that lives in its own
`recovery` partition (`ota_2`). Together with a custom bootloader it turns the
receiver into a **dual-boot** device: two independent firmwares can be kept in
`app0` and `app1`, and the boot manager lets you choose which one to run at every
power-on.

> **Hardware: ESP32-S3 N16R8 only.** Tested only on modules with **16 MB flash
> and 8 MB OPI PSRAM (N16R8)**. Other variants (N8R2, N8R8, N16R2, ...) have
> **not been tested**.

## Why dual-boot?

Keeping two firmwares on the receiver is useful for:

- Running two different builds (e.g. a stable one and an experimental one) and
  switching between them without re-flashing.
- Testing an unknown firmware safely: the boot manager always runs first, so you
  can always switch back to the working slot or re-flash the other one.
- Updating one slot over WiFi while the other keeps working.

## Boot flow

```
power on
   |
   v
bootloader  (custom: prefers the boot manager slot unless a one-shot boot is requested)
   |
   v
boot manager (ota_2)  -- 1s window, encoder held? --> boot manager menu
   | no (auto)
   v
app0 (ota_0) / app1 (ota_1)   (selected firmware)
```

The boot manager starts a firmware as a **one-shot** boot: it sets the target
partition via `esp_ota_set_boot_partition()`, which marks it `ESP_OTA_IMG_NEW`.
The bootloader treats a `NEW` target as a one-shot boot and runs it; once the
firmware confirms itself (`esp_ota_mark_app_valid_cancel_rollback()`) or fails,
the next boot returns to the boot manager.

## Menu

- **Boot App0** / **Boot App1** — boot the selected firmware
- **Firmware Update** — flash a `.bin` from LittleFS into `app0` or `app1`
- **Factory Reset** — erase NVS (settings) and LittleFS (files); the bootloader
  and firmware partitions are kept
- **WiFi File Manager** — upload/delete files over WiFi
  (SSID `ats-recovery`, password `12345678`, http://192.168.4.1)

## Building

The boot manager is a plain Arduino sketch for the ESP32-S3 with the same
libraries as the main firmware. Build it with the board settings used by the
project:

```
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,CPUFreq=80,USBMode=hwcdc,FlashMode=qio,PartitionScheme=custom,DebugLevel=none" \
  --export-binaries ats-mini-recovery
```

The resulting `ats-mini-recovery.ino.bin` is written to the `recovery` partition.

## Flashing

Flash only the boot manager partition (leaves `app0`/`app1`/LittleFS untouched):

```
esptool --chip esp32s3 --port COMx write-flash 0x610000 ats-mini-recovery.ino.bin
```

## License

MIT. See the repository `LICENSE`.
