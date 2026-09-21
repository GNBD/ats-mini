# ATS Mini Recovery

A standalone recovery firmware for the ATS Mini (ESP32-S3 + SI4732) that lives in
its own `recovery` partition (`ota_2`). Combined with a custom bootloader, it
makes the receiver **always boot the recovery first**, no matter what firmware is
flashed into `app0`/`app1`.

> **Hardware: ESP32-S3 N16R8 only.** Tested only on modules with **16 MB flash
> and 8 MB OPI PSRAM (N16R8)**. Other variants (N8R2, N8R8, N16R2, ...) have
> **not been tested**.

## Why a separate recovery partition?

The stock firmware boots straight into the application, so a broken or
incompatible application can make the receiver unusable until it is re-flashed
over USB. With a dedicated recovery partition plus a bootloader that prefers it:

- The recovery always runs first and can re-flash `app0`/`app1` from a file or
  over WiFi, even if the application is broken.
- Any firmware can be put into `app0`/`app1` without losing access to recovery.

## Boot flow

```
power on
   |
   v
bootloader  (custom: forces recovery unless the selected slot is a one-shot boot)
   |
   v
recovery (ota_2)  -- 1s window, encoder held? --> recovery menu
   | no
   v
app0 (ota_0) / app1 (ota_1)   (selected application)
```

The recovery starts an application as a **one-shot** boot: it sets the target
partition via `esp_ota_set_boot_partition()`, which marks it `ESP_OTA_IMG_NEW`.
The bootloader treats a `NEW` target as a one-shot boot and runs it; once the
application confirms itself (`esp_ota_mark_app_valid_cancel_rollback()`) or
fails, the next boot returns to recovery.

## Recovery menu

- **Boot App0** / **Boot App1** — boot the selected application
- **Firmware Update** — flash a `.bin` from LittleFS into `app0` or `app1`
- **Factory Reset** — erase NVS (settings) and LittleFS (files); the bootloader
  and firmware partitions are kept
- **WiFi File Manager** — upload/delete files over WiFi
  (SSID `ats-recovery`, password `12345678`, http://192.168.4.1)

## Building

The recovery is a plain Arduino sketch for the ESP32-S3 with the same libraries
as the main firmware. Build it with the board settings used by the project:

```
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,CPUFreq=80,USBMode=hwcdc,FlashMode=qio,PartitionScheme=custom,DebugLevel=none" \
  --export-binaries ats-mini-recovery
```

The resulting `ats-mini-recovery.ino.bin` is written to the `recovery` partition.

## Flashing

Flash only the recovery partition (leaves `app0`/`app1`/LittleFS untouched):

```
esptool --chip esp32s3 --port COMx write-flash 0x610000 ats-mini-recovery.ino.bin
```

## License

MIT. See the repository `LICENSE`.
