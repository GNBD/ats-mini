# ATS Mini

![](docs/source/_static/esp32-si4732-ui-theme.jpg)

> **⚠️ Work in progress** — This firmware is still under development. Crashes,
> boot failures, data loss, etc. may occur. Use at your own risk.

This firmware is for use on the SI4732 (ESP32-S3) Mini/Pocket Receiver

한국어 문서: [README.md](README.md)

Based on the following sources:

* Volos Projects:    https://github.com/VolosR/TEmbedFMRadio
* PU2CLR, Ricardo:   https://github.com/pu2clr/SI4735
* Ralph Xavier:      https://github.com/ralphxavier/SI4735
* Goshante:          https://github.com/goshante/ats20_ats_ex
* G8PTN, Dave:       https://github.com/G8PTN/ATS_MINI

## dual-boot (this fork)

> **Hardware: ESP32-S3 N16R8 only.** This fork has been tested only on modules
> with **16 MB flash and 8 MB OPI PSRAM (N16R8)**. Other variants (N8R2, N8R8,
> N16R2, ...) have **not been tested**. The partition table and custom bootloader
> assume 16 MB flash and OPI PSRAM; using them on other hardware may not boot.

This fork turns the receiver into a **dual-boot** device. A small **boot manager**
runs first on every power-on and lets you choose which firmware to boot:

```
power on -> boot manager (ota_2) -> app0  (firmware A)
                                 -> app1  (firmware B)
```

* Two independent firmwares can be kept in `app0` and `app1` and switched at
  boot, without re-flashing.
* The boot manager can flash a new firmware into either slot (from a file or over
  WiFi), so one slot can be updated while the other keeps working.
* Even if one firmware is broken, the boot manager still runs, so you can always
  switch to the other slot or re-flash.

See:

* [ats-mini-recovery/README.en.md](ats-mini-recovery/README.en.md) - boot manager firmware
* [ats-mini/bootloader.en.md](ats-mini/bootloader.en.md) - custom bootloader and how to build it
* [ats-mini/partitions.csv](ats-mini/partitions.csv) - 16 MB partition layout

## Releases

Check out the [Releases](https://github.com/esp32-si4732/ats-mini/releases) page.

## Documentation

The hardware, software and flashing documentation is available at <https://esp32-si4732.github.io/ats-mini/>
