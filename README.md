# ATS Mini Dualboot

> **⚠️ 미완성 (work in progress)** — 이 펌웨어는 아직 개발 중입니다. 크래시,
> 부팅 실패, 데이터 손실 등이 발생할 수 있습니다. 사용에 주의하세요.

SI4732 (ESP32-S3) Mini/Pocket 수신기용 펌웨어입니다.

다음 소스를 기반으로 합니다:

* Volos Projects:    https://github.com/VolosR/TEmbedFMRadio
* PU2CLR, Ricardo:   https://github.com/pu2clr/SI4735
* Ralph Xavier:      https://github.com/ralphxavier/SI4735
* Goshante:          https://github.com/goshante/ats20_ats_ex
* G8PTN, Dave:       https://github.com/G8PTN/ATS_MINI

## dual-boot (이 포크)
<img width="885" height="567" alt="image" src="https://github.com/user-attachments/assets/8d64166c-15dc-4acb-8fe8-5a2c3f4aea3c" />

> **하드웨어: ESP32-S3 N16R8 전용.** 이 포크는 **16MB flash + 8MB OPI PSRAM
> (N16R8)** 모듈에서만 테스트했습니다. 다른 버전(N8R2, N8R8, N16R2 등)은
> **테스트하지 않았습니다.** 파티션 테이블과 커스텀 부트로더가 16MB flash와
> OPI PSRAM을 전제로 하므로, 다른 하드웨어에서는 부팅되지 않을 수 있습니다.

이 포크는 수신기를 **dual-boot** 기기로 만듭니다. 전원을 켤 때마다 작은
**부트 매니저**가 먼저 실행되어, 부팅할 펌웨어를 선택할 수 있습니다:

```
전원 ON -> 부트 매니저 (ota_2) -> app0  (펌웨어 A)
                              -> app1  (펌웨어 B)
```

* 서로 다른 두 펌웨어를 `app0`과 `app1`에 넣고, 재플래시 없이 부팅 시 전환할 수 있습니다.
* 부트 매니저가 각 슬롯에 새 펌웨어를 플래시할 수 있어(파일 또는 WiFi), 한쪽을 업데이트하는 동안 다른 쪽은 그대로 사용할 수 있습니다.
* 한쪽 펌웨어가 망가져도 부트 매니저는 항상 실행되므로, 다른 슬롯으로 전환하거나 다시 플래시할 수 있습니다.

참고:

* [ats-mini-recovery/README.md](ats-mini-recovery/README.md) - 부트 매니저 펌웨어
* [ats-mini/bootloader.md](ats-mini/bootloader.md) - 커스텀 부트로더와 빌드 방법
* [ats-mini/partitions.csv](ats-mini/partitions.csv) - 16MB 파티션 레이아웃

ENG: [README.en.md](README.en.md)

## Releases

[Releases](https://github.com/esp32-si4732/GNBD/releases) 페이지를 참고하세요.

## Documentation

하드웨어, 소프트웨어, 플래시 문서는 <https://esp32-si4732.github.io/ats-mini/> 에 있습니다.
