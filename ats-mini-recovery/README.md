# ATS Mini 부트 매니저 (dual-boot)

> **⚠️ 미완성 (work in progress)** — 아직 개발 중입니다. 크래시, 부팅 실패,
> 데이터 손실 등이 발생할 수 있습니다. 사용에 주의하세요.

ATS Mini(ESP32-S3 + SI4732)용 소형 부트 매니저입니다. 자체 `recovery` 파티션
(`ota_2`)에 저장되며, 커스텀 부트로더와 함께 수신기를 **dual-boot** 기기로
만듭니다. 서로 다른 두 펌웨어를 `app0`과 `app1`에 넣고, 전원을 켤 때마다
부트 매니저에서 어느 것을 실행할지 선택할 수 있습니다.

> **하드웨어: ESP32-S3 N16R8 전용.** **16MB flash + 8MB OPI PSRAM (N16R8)**
> 모듈에서만 테스트했습니다. 다른 버전(N8R2, N8R8, N16R2 등)은
> **테스트하지 않았습니다.**

## 왜 dual-boot인가

수신기에 두 개의 펌웨어를 두면 다음과 같은 장점이 있습니다:

- 서로 다른 빌드(예: 안정판 + 실험판)를 넣고 재플래시 없이 전환할 수 있습니다.
- 검증되지 않은 펌웨어를 안전하게 테스트할 수 있습니다. 부트 매니저가 항상 먼저
  실행되므로, 문제가 생기면 다른 슬롯으로 전환하거나 다시 플래시할 수 있습니다.
- 한 슬롯을 WiFi로 업데이트하는 동안 다른 슬롯은 그대로 사용할 수 있습니다.

## 부팅 흐름

```
전원 ON
   |
   v
부트로더  (커스텀: one-shot 부팅이 아니면 부트 매니저 슬롯 우선)
   |
   v
부트 매니저 (ota_2)  -- 1초 대기, encoder 누름? --> 부트 매니저 메뉴
   | 아니오 (자동)
   v
app0 (ota_0) / app1 (ota_1)   (선택된 펌웨어)
```

부트 매니저는 펌웨어를 **one-shot**으로 부팅합니다. `esp_ota_set_boot_partition()`로
대상 파티션을 지정하면 상태가 `ESP_OTA_IMG_NEW`가 되고, 부트로더는 이를
one-shot 부팅으로 간주해 실행합니다. 펌웨어가 스스로 확인
(`esp_ota_mark_app_valid_cancel_rollback()`)하거나 실패하면, 다음 부팅은 다시
부트 매니저로 돌아옵니다.

## 메뉴

- **Boot App0** / **Boot App1** — 선택한 펌웨어 부팅
- **Firmware Update** — LittleFS의 `.bin` 파일을 `app0` 또는 `app1`에 플래시
- **Factory Reset** — NVS(설정)와 LittleFS(파일) 삭제, 부트로더/펌웨어는 유지
- **WiFi File Manager** — WiFi로 파일 업로드/삭제
  (SSID `ats-recovery`, 비밀번호 `12345678`, http://192.168.4.1)

## 빌드

부트 매니저는 메인 펌웨어와 같은 라이브러리를 쓰는 ESP32-S3용 Arduino
스케치입니다. 프로젝트와 동일한 보드 설정으로 빌드합니다:

```
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,CPUFreq=80,USBMode=hwcdc,FlashMode=qio,PartitionScheme=custom,DebugLevel=none" \
  --export-binaries ats-mini-recovery
```

생성된 `ats-mini-recovery.ino.bin`을 `recovery` 파티션에 기록합니다.

## 플래시

부트 매니저 파티션만 플래시합니다 (`app0`/`app1`/LittleFS는 유지):

```
esptool --chip esp32s3 --port COMx write-flash 0x610000 ats-mini-recovery.ino.bin
```

영어 문서: [README.en.md](README.en.md)

## 라이선스

이 프로젝트의 원본 코드는 적용되는 범위에서 MIT 라이선스에 따라 제공됩니다.

다만 이 디렉터리 또는 빌드 결과물에 포함되는 제3자 구성요소에는
GPL-3.0, LGPL 또는 기타 별도 라이선스가 적용될 수 있습니다.

특히 로터리 엔코더 구현의 라이선스와 저작권 고지는 저장소의
`NOTICE` 및 `LICENSES/` 디렉터리를 확인하세요.

소스 코드나 펌웨어 바이너리를 재배포하기 전에는 해당 구성요소의
라이선스 조건을 확인하고 필요한 저작권·라이선스 고지를 함께 제공해야 합니다.
