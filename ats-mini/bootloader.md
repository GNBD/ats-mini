# Custom bootloader (recovery-first boot)

`ats-mini/bootloader.bin` is a modified ESP-IDF second-stage bootloader. When it
is present in the sketch directory, the Arduino ESP32 build copies it as the
bootloader instead of the stock one.

## What it changes

The stock bootloader boots whatever OTA slot the `otadata` partition points to.
The ATS Mini application normally sets the recovery slot (`ota_2`) as the next
boot target itself, so the recovery-first behaviour breaks as soon as a firmware
that does not do that is flashed into `app0`/`app1`.

The modified bootloader enforces the order in `bootloader_utility_get_selected_boot_partition()`:

- If the selected OTA slot is not `ota_2` and its `otadata` state is not
  `ESP_OTA_IMG_NEW`, boot `ota_2` (the recovery) instead.
- Otherwise keep the normal selection. The recovery starts an application with
  `esp_ota_set_boot_partition()`, which marks it `ESP_OTA_IMG_NEW`, so it is
  allowed to run as a one-shot boot.
- When `otadata` is empty, boot `ota_2` if the recovery partition exists.

Result: `power on -> recovery -> application`, regardless of the firmware in
`app0`/`app1`.

## Building

The bootloader must match the ESP-IDF version and flash configuration used by the
Arduino core. This project targets **ESP-IDF v5.5.5** and the settings in
`sketch.yaml` (`FlashSize=16M`, `PSRAM=opi`, `FlashMode=qio`, `CPUFreq=80`).

1. Install ESP-IDF v5.5.5.
2. Apply `bootloader.patch` to the ESP-IDF source:
   ```
   cd $IDF_PATH
   git apply /path/to/ats-mini/bootloader.patch
   ```
3. Build the bootloader for a project whose `sdkconfig` matches the board
   settings above, then copy the result to `ats-mini/bootloader.bin`:
   ```
   idf.py bootloader
   cp build/bootloader/bootloader.bin /path/to/ats-mini/bootloader.bin
   ```
4. Rebuild the firmware with `arduino-cli`; the custom bootloader is copied
   automatically (see the `recipe.hooks.prebuild` rule in the ESP32 core's
   `platform.txt`).

## Notes

- **Hardware: ESP32-S3 N16R8 only.** The bootloader was built for **16 MB flash
  and 8 MB OPI PSRAM (N16R8)**. Other variants have not been tested.
- The patch only touches `components/bootloader_support/src/bootloader_utility.c`.
- `bootloader.bin` is committed so that CI and normal builds use it without
  requiring an ESP-IDF installation.
- The bootloader is ESP-IDF code and is distributed under the Apache License 2.0
  (see the repository `NOTICE`).
