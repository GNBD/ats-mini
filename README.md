# ATS Mini

![](docs/source/_static/esp32-si4732-ui-theme.jpg)

This firmware is for use on the SI4732 (ESP32-S3) Mini/Pocket Receiver

Based on the following sources:

* Volos Projects:    https://github.com/VolosR/TEmbedFMRadio
* PU2CLR, Ricardo:   https://github.com/pu2clr/SI4735
* Ralph Xavier:      https://github.com/ralphxavier/SI4735
* Goshante:          https://github.com/goshante/ats20_ats_ex
* G8PTN, Dave:       https://github.com/G8PTN/ATS_MINI

## Recovery-first boot (this fork)

> **Hardware: ESP32-S3 N16R8 only.** This fork has been tested only on modules
> with **16 MB flash and 8 MB OPI PSRAM (N16R8)**. Other variants (N8R2, N8R8,
> N16R2, ...) have **not been tested**. The partition table and custom bootloader
> assume 16 MB flash and OPI PSRAM; using them on other hardware may not boot.

This fork adds a dedicated **recovery partition** and a custom bootloader so the
receiver always boots the recovery first:

```
power on -> recovery (ota_2) -> application (app0/app1)
```

* The recovery can re-flash `app0`/`app1` from a file or over WiFi, even if the
  application is broken.
* Any firmware can be placed in `app0`/`app1` without losing access to recovery.

See:

* [ats-mini-recovery/README.md](ats-mini-recovery/README.md) - recovery firmware
* [ats-mini/bootloader.md](ats-mini/bootloader.md) - custom bootloader and how to build it
* [ats-mini/partitions.csv](ats-mini/partitions.csv) - 16 MB partition layout

## Releases

Check out the [Releases](https://github.com/esp32-si4732/ats-mini/releases) page.

## Documentation

The hardware, software and flashing documentation is available at <https://esp32-si4732.github.io/ats-mini/>

