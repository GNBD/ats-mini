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
<img width="657" height="350" alt="image" src="https://github.com/user-attachments/assets/a04bad85-3edb-43bd-9c66-b7643d7b9e33" />


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
* [ats-mini/partitions.csv](ats-mini/partitions.csv) - 16MB 파티션 레이아웃

ENG: [README.en.md](README.en.md)

## Releases

[Releases](https://github.com/GNBD/ats-mini-dualboot/releases) 페이지를 참고하세요.

## Documentation

하드웨어, 소프트웨어, 플래시 문서는 <https://esp32-si4732.github.io/ats-mini/> 에 있습니다.


## 라이선스 및 제3자 고지

이 저장소에는 자체 작성 코드와 제3자 구성요소가 함께 포함되어 있습니다.

특정 파일 또는 디렉터리에 별도의 저작권·라이선스 고지가 없는 경우,
이 저장소를 위해 작성된 원본 코드는 [MIT](LICENSE) 라이선스에 따라
제공됩니다. 제3자 구성요소에는 각 구성요소의 별도 저작권 및
라이선스 조건이 적용됩니다.

**펌웨어 바이너리:** GPL-3.0 코드(Rotary)와 링크되므로, 배포되는
펌웨어 바이너리는 결합 저작물에 대해 **GPL-3.0** 조건이 적용됩니다.
바이너리 배포 시 해당 커밋/태그의 소스 코드를 함께 제공해야 합니다.

특히 다음 사항을 확인해야 합니다.

* `ats-mini/Rotary.cpp` 및 `ats-mini/Rotary.h`에는 Ben Buxton의
  로터리 엔코더 구현이 포함되어 있으며 **GPL-3.0** 조건이 적용됩니다.
  전문: [LICENSES/GPL-3.0.txt](LICENSES/GPL-3.0.txt)
* `ats-mini/patch_init.h`의 SSB 패치 데이터는 별도 출처(Vadim Afonkin)
 이며 라이선스가 명확하지 않습니다. [NOTICE](NOTICE) §3.1
* 외부 라이브러리에는 MIT, LGPL 또는 기타 라이선스가 적용될 수 있습니다.
* 하드웨어 설계(Sunnygold 등)에는 **CC BY-NC-SA 3.0**(비상업)이 적용될 수 있습니다.
  전문: [LICENSES/CC-BY-NC-SA-3.0.txt](LICENSES/CC-BY-NC-SA-3.0.txt)
* Volos Projects(TEmbedFMRadio)는 **라이선스 파일이 없음** — 복사·재배포 시 주의.

소스 코드, 펌웨어 바이너리, 하드웨어 파일 또는 문서를 재배포하기 전에는
[LICENSE](LICENSE), [NOTICE](NOTICE), [LICENSES/](LICENSES/) 및 각 파일의
라이선스 고지를 확인해야 합니다.

별도로 명시하지 않는 한, 이 프로젝트는 원본 프로젝트 또는 하드웨어 설계
저작자의 공식 제품이 아닙니다.
