# edge-lpr — 카메라 위에서 완결되는 한국 번호판 인식

한화비전 **PNM-C16083RVQ** 멀티디렉셔널 카메라에서 도는 Wisenet Open Platform(OpenSDK) 엣지 앱.
서버·GPU 없이 **카메라 안에서** 번호판 검출 → OCR → 교차검증 → 확정까지 끝낸다.

- 검출: WiseAI 번호판 박스 + 카메라가 미리 잘라주는 good-shot 크롭(`ImageRef`)
- 인식: [tinyLPR](https://github.com/noahzhy/KR_LPR) 87KB TFLite 모델 (CTC, 192×96 gray)
- 확정: 멀티후보 판독 + 신뢰도 챔피언십 + 증거등급 게이트 — **"틀리게 확정하느니 침묵한다"**
- 제약: 앱에 CPU 2코어만 허용, SDK 26.05.19 / SoC cv5

## 성적 (2026-07-28 v10 기준선)

카메라가 모니터 재생 영상(번호판 8종 순환)을 실촬영하는 조건:

| 지표 | 값 |
|---|---|
| ★ FINAL(확정) 정답률 | **11/11 (오확정 0)** |
| ? HOLD(보류) | 0건 — 이전 런들에서도 보류의 대부분은 오독의 정당한 억류 |
| conf 1.00 비율 | 11개 중 10개 |

```
OCR start ch2 #97 crop=608x232 sharp 294
  tinyLPR(6 cands): "25누5701" (conf 1.00, 24.1ms) | lum 101 | color wh
★ PLATE FINAL ch2 id1781 -> "25누5701" (best-of-2, conf 1.00, src=good-shot)
```

## 어떻게 동작하나 — 3계층

### 1) 캡처 (카메라 튜닝이 절반이다)

| 설정 | 값 | 왜 |
|---|---|---|
| 셔터 | 1/480 고정 | 모션블러 제거 (블러의 지문 = "227" 식 앞자리 겹침) |
| 밝기(AE 목표) | 55 | 게인 노이즈 억제 |
| AGC | 중간 | 노이즈↔밝기 균형점 |
| SSNR | **끔** | 시간축 NR은 움직이는 번호판에 이중상(고스팅) 유발 |
| 역광보정 | BLC (번호판 통과 구역) | 헤드라이트에 속는 AE 교정. WDR은 다중노출 고스팅이라 금지 |

### 2) 인식 (tinyLPR 멀티후보)

good-shot 1장을 그대로 읽지 않고 **6~8개 공간 후보**(원본 + 밝은영역 박스 ×여백 2종 + 중앙 크롭 3종)로
잘라 전부 판독 → 유효 포맷 우선·conf 최고 승리. 색-한글 검증(아바사자배=노랑판 전용)으로 환각 기각.

### 3) 중재 (신뢰도 챔피언십 + 게이트)

- good-shot(1표) + 버스트 샘플(최대 12표, 150ms 스로틀)을 텍스트별 그룹핑, `max(conf) + 반복보너스 + primary 가중`으로 우승 결정
- **FINAL 게이트 0.96**: 미달이면 HOLD(침묵). 교차합의(good-shot+버스트 동일 텍스트)는 0.97 승격, 버스트 단독 <0.98은 강등
- **가공 격리 원칙(사고 5회로 확립)**: 지터·조도·디노이즈·샤프닝 등 가공 판독은 전부 **캡 0.95** + 원판독이 게이트 미달일 때만 개입.
  *게이트를 넘는 원본 판독은 불가침, 가공은 어차피 HOLD인 판만 구제한다.*

## 빌드 / 배포

```bash
SDK_VER=26.05.19 SOC=cv5 APP_NAME=object_detect docker compose up   # → object_detect.cap
```

카메라 웹 UI → 설정 → 오픈플랫폼 → `.cap` 업로드 → Start.
(패키징 제약 때문에 앱 ID는 `object_detect`를 유지한다 — 레포 이름과 다름)

- 라이브 확인: `http://<카메라IP>/opensdk/object_detect/` 웹 UI (감지 박스 + 저장된 번호판 갤러리)
- 판독 결과 API: `GET /opensdk/object_detect/platetext?ch=N`
- 실시간 로그: `CLI/remote_debug_viewer` (PC에서 실행, DebugHelper 앱 필요)

## 문서 지도

| 문서 | 내용 |
|---|---|
| [OCR_JOURNEY.md](OCR_JOURNEY.md) | **번호판 인식 개발 전 여정** — 17%→100%까지의 실험·사고·교훈 |
| [MOTION_DETECT.md](MOTION_DETECT.md) | 토대가 된 움직임 감지 앱 구현 + OpenSDK 트러블슈팅 |
| [BASICS.md](BASICS.md) | OpenSDK 용어·개념을 비유로 설명 |
| [DATAFLOW.md](DATAFLOW.md) | 실제 로그 숫자로 따라가는 데이터 흐름 |
| [FROM_SCRATCH.md](FROM_SCRATCH.md) | 깡통 프로젝트 → 이 앱까지 단계별 재구성 |
| [결과보고.html](결과보고.html) | 실험 보고서 (Confluence용) |

## 남은 과제

- 등록차량 DB 최근접 매칭 레이어 (HOLD 회수 → 실환경 95%+ 경로)
- 연산 부하 최적화 (버스트 경량 파이프라인, TFLite 2스레드)
