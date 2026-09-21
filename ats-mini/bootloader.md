# 커스텀 부트로더 (dual-boot)

`ats-mini/bootloader.bin`은 수정된 ESP-IDF 2단계 부트로더입니다. 스케치 폴더에
있으면 Arduino ESP32 빌드가 기본 부트로더 대신 이 파일을 사용합니다. 이
부트로더가 전원을 켤 때마다 부트 매니저(`ota_2`)를 먼저 실행시키며, 이것이
dual-boot을 가능하게 합니다.

## 무엇을 바꾸나

기본 부트로더는 `otadata` 파티션이 가리키는 OTA 슬롯을 부팅합니다. 부트
매니저는 보통 스스로 `ota_2` 슬롯을 다음 부팅 대상으로 지정하지만, 그렇게 하지
않는 펌웨어가 `app0`/`app1`에 올라가면 동작이 깨집니다. 수정된 부트로더는
`bootloader_utility_get_selected_boot_partition()`에서 부트 매니저를 강제합니다:

- 선택된 OTA 슬롯이 `ota_2`가 아니고 `otadata` 상태가 `ESP_OTA_IMG_NEW`가
  아니면, 대신 `ota_2`(부트 매니저)를 부팅합니다.
- 그 외에는 정상 선택을 따릅니다. 부트 매니저가 `esp_ota_set_boot_partition()`로
  펌웨어를 시작하면 상태가 `ESP_OTA_IMG_NEW`가 되므로, one-shot 부팅으로
  허용됩니다.
- `otadata`가 비어 있으면, 부트 매니저 파티션이 있을 때 `ota_2`를 부팅합니다.

결과: `app0`/`app1`에 어떤 펌웨어가 있든 `전원 ON -> 부트 매니저 -> 펌웨어`
순서로 동작합니다.

## 빌드

부트로더는 Arduino 코어가 쓰는 ESP-IDF 버전과 flash 설정에 맞아야 합니다. 이
프로젝트는 **ESP-IDF v5.5.5**와 `sketch.yaml`의 설정(`FlashSize=16M`,
`PSRAM=opi`, `FlashMode=qio`, `CPUFreq=80`)을 기준으로 합니다.

1. ESP-IDF v5.5.5를 설치합니다.
2. ESP-IDF 소스에 `bootloader.patch`를 적용합니다:
   ```
   cd $IDF_PATH
   git apply /path/to/ats-mini/bootloader.patch
   ```
3. 위 보드 설정과 일치하는 `sdkconfig`를 가진 프로젝트에서 부트로더를 빌드한 뒤,
   결과물을 `ats-mini/bootloader.bin`으로 복사합니다:
   ```
   idf.py bootloader
   cp build/bootloader/bootloader.bin /path/to/ats-mini/bootloader.bin
   ```
4. `arduino-cli`로 펌웨어를 다시 빌드하면 커스텀 부트로더가 자동으로
   사용됩니다(ESP32 코어 `platform.txt`의 `recipe.hooks.prebuild` 규칙).

## 참고

- **하드웨어: ESP32-S3 N16R8 전용.** **16MB flash + 8MB OPI PSRAM (N16R8)**
  기준으로 빌드했습니다. 다른 버전은 테스트하지 않았습니다.
- patch는 `components/bootloader_support/src/bootloader_utility.c`만 수정합니다.
- `bootloader.bin`을 저장소에 커밋해 두어, ESP-IDF 설치 없이도 CI와 일반
  빌드에서 사용됩니다.
- 부트로더는 ESP-IDF 코드이며 Apache License 2.0을 따릅니다.

영어 문서: [bootloader.en.md](bootloader.en.md)
