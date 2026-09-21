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

## Discuss

* [GitHub Discussions](https://github.com/esp32-si4732/ats-mini/discussions) - the best place for feature requests, observations, sharing, etc.
* [TalkRadio Telegram Chat](https://t.me/talkradio/174172) - informal space to chat in Russian and English.
