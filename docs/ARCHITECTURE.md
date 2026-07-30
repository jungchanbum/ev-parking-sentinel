# edge-lpr (object_detect) 아키텍처 — 동작 원리 전해부

> Hanwha Vision PNM-C16083RVQ 위에서 **온카메라(서버 없음)** 로 한국 번호판을 읽어
> ★FINAL(확정) / ?HOLD(보류) 를 발급하는 OpenSDK 앱.
> 실측 기준(야간·컬러고정·골든 파라미터): **오확정 0 / 판정 발급률 96~100%**, 판독 15~60ms.
>
> 관련 문서: [카메라 골든 파라미터](../config/CAMERA_SETTINGS.md) · [구버전 README(모션 감지 시절)](../MOTION_DETECT.md)

---

## 0. 한 문장 요약

**카메라(WiseAI)가 번호판을 감지·크롭해주면, 앱이 그 크롭 1장(굿샷)과 라이브 프레임 최대 3장(버스트)을
초경량 OCR(tinyLPR, 87KB)로 읽고, "즉시확정 → 챔피언십 투표 → 등록차량 DB 그물"의 3단 판정을 거쳐
확실한 것만 ★FINAL 로 발급하고, 못 미더운 건 ?HOLD 로 침묵한다.**

정확도의 비밀은 모델이 아니라 **판정 구조**다: 오독은 반드시 나온다는 전제 아래,
오독이 FINAL 까지 살아 나가지 못하게 관문을 겹겹이 세웠다.

---

## 1. 실행 환경

| 항목 | 값 |
|---|---|
| 카메라 | PNM-C16083RVQ (멀티디렉셔널 4센서, Ambarella CV5) |
| 플랫폼 | Hanwha OpenSDK 26.05.19 / SOC=cv5, 앱은 컨테이너(cgroup)로 격리 |
| CPU 예산 | **2코어 고정** (cgroup) — 모든 성능 설계의 제1 제약 |
| 동거인 | WiseAI(감지·크롭 담당, NPU 사용), CloudConnector, DebugHelper |
| OCR 모델 | tinyLPR (noahzhy/KR_LPR) — 입력 192×96 gray, CTC, **87KB** TFLite |
| 빌드 | `SDK_VER=26.05.19 SOC=cv5 APP_NAME=object_detect docker compose up` → `object_detect.cap` |
| 배포 | 웹 UI 오픈플랫폼 or SUNAPI CGI 설치. **설치는 영상 정지 + 재부팅 직후** (임시공간 고갈 시 OpenSDKError 105) |

앱은 SDK 의 `SampleComponent` 컴포넌트로 로드되어 이벤트 루프 안에서 산다.
스레드는 사실상 1개(이벤트 콜백) + SDK 내부 스레드들. TFLite 도 1스레드
(2스레드는 스레드풀 스핀이 2코어를 독점해 앱 동결 — 실측 낙제).

---

## 2. 전체 데이터 플로우

```
                        ┌─────────────────────────── 카메라(WiseAI, NPU) ───────────────────────────┐
                        │  객체 감지/추적 → ONVIF 메타데이터 XML (30fps)                              │
                        │  번호판 best-shot 크롭 JPEG → /tmp/download/chN/..._objectid_<oid>_*.jpg   │
                        └───────────────┬───────────────────────────────┬──────────────────────────┘
                                        │ eMetadataRequest (XML)        │ <tt:ImageRef> 경로
                                        ▼                               │
     SampleComponent::HandleMetadataEvent → ProcessObjects(ch, xml)     │
                                        │                               │
              ┌─────────────────────────┼───────────────────────────────┘
              │                         │
              ▼                         ▼
   [core/metadata_parser.h]   [core/plate_store.h]
   XML → meta::Frame           ImageRef JPEG 검증(FFD8..FFD9)·저장
   (Human/Vehicle/Plate,       → ../storage/cap_<slot>.jpg
    bbox·중심좌표·imgref)       (부분파일이면 pending 재시도, 최대 3s)
              │                         │ "plate SAVE #n"
              ▼                         ▼
   [core/motion_tracker.h]     RecognizePlate(ch, slot)  ◄─── 굿샷 경로(정예 1장)
   움직임 판정                     │  크기 검문(300×176 미만 거부)
   "moving vehicle!" 이벤트        │  선명도(sharp) 측정
              │                    ▼
              │            [ocr/plate_ocr.h] Recognize(full)
              │              4후보 토너먼트 → 지터 → 조도 정규화 → (구조대 off)
              │                    │ "tinyLPR(4 cands) ..."/"OCR result ..."
              │                    ▼
              │      ┌── conf ≥ 0.985 & 무반박? ──────────────┐
              │      │ YES: ★instant FINAL (db-fix 후 발급)    │ NO: 투표함에 표 제출
              │      │      plate_done_ 등록 → 이후 연산 전면 차단
              │      └────────────────────────────────────────┘
              │
              ├─ 번호판 bbox 프레임마다 → BurstSample(...)  ◄─── 버스트 경로(물량 3장)
              │      웜업 1s + 150ms 스로틀 + 차당 3장 (PlateVote::CanSample)
              │      스냅샷 주문(RefreshSnapshot) → bbox+여백 크롭
              │      → [plate_ocr.h] Recognize(light: 3후보만)
              │      → conf<0.90 환각 폐기, 그 외 투표함에 표 제출  "burst ... sample#k"
              │
              └─ 매 프레임: FinalizeStalePlates(전 채널)
                     트랙 만료(15프레임 or 3초, primary 없으면 ×3 유예) 시 개표:
                     [ocr/plate_vote.h] Finalize — 신뢰도 챔피언십
                       score = max(conf) + 0.02×min(표수-1,3) + 굿샷 가중 0.02
                       한글 접전 캡 0.94 / 버스트 단독 캡 / 교차합의 승격 0.97
                             │
                             ▼  게이트 conf ≥ 0.95
                     [ocr/plate_db.h] 등록차량 DB 그물 (편집거리≤1 유일 매칭)
                       통과 → db-fix(교정) / 미달 → db-rescue(0.80/0.85 회수)
                       지역명 환각("경기36라7833")은 벗겨서 재대조
                             │
                 ┌───────────┴───────────┐
                 ▼                       ▼
          ★ PLATE FINAL           ? PLATE HOLD (확정 발급 거부 = 오확정 0 의 비결)
                 │
                 ▼
   [io/*_sink.h] 이벤트 출구: DebugViewerSink(원격 로그) + DiskLogSink(events.log)
   [HTTP] /detections, /platetext, /lastplate ... ← 외부 시스템(이더넷)이 조회
```

---

## 3. 파일별 완전 해부

### 3.1 `sample_component.h` / `sample_component.cc` — 오케스트레이터 (946줄)

앱의 유일한 SDK 접점이자 지휘자. **도메인 로직은 전부 헤더 모듈로 위임**하고,
자신은 이벤트 수신 → 모듈 호출 → 결과 발행만 한다.

| 함수 | 역할 |
|---|---|
| `Initialize()` | sink 2개 등록(DebugViewer/Disk), HTTP URI 등록, tinyLPR 모델 로드, **등록차량 DB 로드**(파일 없으면 순수 인식 모드) |
| `ProcessAEvent()` | SDK 이벤트 분기: 메타데이터 / HTTP / 앱정보(AppId) |
| `HandleMetadataEvent()` | XML 꺼내 `ProcessObjects(ch, xml)` 호출 |
| `ProcessObjects()` | **프레임당 메인 루프.** 파서 호출 → 사람/차량/번호판 분류(detect on/off 필터) → 움직임 판정·이벤트 → 번호판 신규 알림 → PlateStore 저장 → RecognizePlate → BurstSample → RetryPending → `/detections`용 latest_ 갱신 → **전 채널 FinalizeStalePlates**(끊긴 채널의 시계를 살아있는 채널이 대신 돌림 — 영상 끝나도 확정 보장) |
| `RecognizePlate(ch, slot)` | 굿샷 정밀 판독. 크기 검문(300×176) → sharp 측정(라플라시안 분산) → `PlateOcr::Recognize(full)` → **즉시확정 게이트**: conf≥0.985 & 유효포맷 & 버스트 반박(0.98+) 없음 → db-fix 후 ★FINAL + `plate_done_` 등록(그 차의 남은 연산 전부 차단). 아니면 투표함에 primary 표 제출(단, sharp<10 물렁 크롭은 primary 가중 박탈) |
| `BurstSample(...)` | 라이브 표 수집. `plate_done_`/스로틀 검사 → 스냅샷에서 bbox+여백(가로35%/세로60%) 크롭 → `Recognize(light)` → conf<0.90 환각 폐기 → 투표함에 표 |
| `FinalizeStalePlates()` | 만료 트랙 개표. primary 없으면 유예 ×3(늦은 굿샷 대기) → (옵션) 버스트 스태킹 평균본 마지막 1표 → `PlateVote::Finalize` → 게이트 → **DB 레이어**(db-fix/db-rescue) → ★FINAL 또는 ?HOLD. FINAL 차는 `plate_done_` 등록, HOLD 는 일부러 안 막음(늦은 굿샷 1.00 승격 기회) |
| `RefreshSnapshot(ch)` | 카메라에 스냅샷 JPEG 주문(100ms 스로틀) + 완성본 캐시 — 스트리밍 없이도 버스트 가능 |
| `RegisterURI()` / `HandleHttpRequest()` | HTTP API 15종 등록·처리 (아래 §5) |
| 말단 `create_component/destroy_component` | SDK 가 .so 를 로드할 때 부르는 C 엔트리포인트 |

핵심 상태(채널별 [4]): `plate_seen_`(트랙 생존 {tick,ms}), `plate_done_`(확정 완료 oid — 이중판정 방지),
`last_final_`, `latest_`(HTTP 오버레이용), `frame_w/h_`, 진단 카운터들.

### 3.2 `includes/config.h` — 설정 단일 출처 (161줄)

모든 튜닝 상수가 실측 근거 주석과 함께 여기 산다. 핵심:

| 상수 | 값 | 의미 |
|---|---|---|
| `kInstantFinalConf` | 0.985 | 즉시확정 문턱 (0.99·1.00만 통과) |
| `kInstantConflictConf` | 0.98 | 즉시확정을 보류시키는 버스트 반박 문턱 |
| `kFinalConfFloor` | 0.95 | 최종 게이트 |
| `kRescueConfCap` | 0.94 | 가공(지터/노출/접전) 결과 캡 — 단독으로 게이트 못 넘음 |
| `kDbRescueExactMin/Ed1Min` | 0.80 / 0.85 | db-rescue 하한 (정확일치/1글자) |
| `kMinOcrCropWidth/Height` | 300 / 176 | 크롭 검문 (176 미만 납작 크롭 = 전량 환각, 실측) |
| `kBurstWarmupMs/ThrottleMs/Max` | 1000 / 150 / 3 | 버스트 다이어트 |
| `kStaleFrames/Ms` | 15 / 3000 | 트랙 만료 (프레임 OR 벽시계 — 영상 끝나도 확정) |
| `kTfliteThreads` | 1 | 2는 스핀 동결(실측 낙제) |
| `kRescueChain` | false | 구조대(샤프닝/디노이즈/채널) 완전 오프 — 부하 다이어트 |
| `kPlateDb/kPlateDbFile` | true / `../res/ocr_models/registered_plates.txt` | DB 레이어 |

### 3.3 `includes/core/metadata_parser.h` — ONVIF XML 파서 (168줄)

**순수 함수 파서.** 문자열 스캔(정규식 없음)을 이 파일에 가둬서, 다른 카메라/포맷으로
이식할 땐 여기만 교체하면 된다. `Parse(xml, fw, fh)` → `meta::Frame`:

- `<tt:Scale>` 에서 프레임 좌표계 크기 복원 (`폭 = 2/|scaleX|`)
- 객체별로 Human / Vehicle / **LicensePlate** 분류, bbox 를 0~1 정규화
- 번호판은 `Parent`(부모 차량 id) + **`<tt:ImageRef>`**(카메라가 저장한 크롭 경로!) 추출
  — 이 ImageRef 발견이 프로젝트의 전환점이었다: 스냅샷도 직접 크롭도 필요 없음
- 사람/차량은 `CenterOfGravity`(중심좌표) 유효한 것만 (움직임 추적용)

### 3.4 `includes/core/motion_tracker.h` — 움직임 추적 (88줄)

객체(id)별 최근 중심좌표 궤적(deque)으로 "움직이는가"를 판정.
`kCompareBack` 프레임 전과의 거리 > `coord_scale × kMoveRatio` 면 moving,
3초(`kMovingHoldMs`) 무움직임이면 해제. **정지→움직임 전환 순간에만** 이벤트 1회
(`moving vehicle!`). 번호판 박스 표시 여부(부모 차량이 움직일 때만)에도 쓰인다.
좌표 스케일 자동 감지 → 정규화 안 된 좌표계도 무설정 대응.

### 3.5 `includes/core/plate_store.h` — 크롭 저장고 (177줄)

카메라가 `/tmp/download/chN/` 에 써주는 best-shot JPEG 을 받아 순환버퍼
(`../storage/cap_<slot>.jpg`, `kRingSize` 슬롯)에 저장.

- **완성 JPEG 검증**: `FFD8 ... FFD9` 시그니처 — 카메라가 아직 쓰는 중인 부분파일이면
  pending 등록 후 `RetryPending`(최대 3초) 재시도. 이게 `imgref=O`(지연 재저장)의 정체
- **중복 제거**: 같은 ImageRef 스킵, 같은 oid 재프레임은 같은 슬롯 덮어쓰기(카운트 X)
- ImageRef 경로에서 `objectid_<oid>` 를 파싱 — 판독 결과를 투표함의 그 차 앞으로 배달하는 열쇠
- 선명도 비교/거부 없음 — "**WiseAI 가 이미 best-shot 을 골라줬다**"는 신뢰가 설계 전제

### 3.6 `includes/ocr/tflite_model.h` — TFLite 래퍼 (71줄)

모델 로드(스레드 수 지정), 입력 HW 조회, `Run(float*, ...)` → 출력 [T,C] 텐서.
tinyLPR 전용 박봉 래퍼 — OCR 로직은 일절 없음.

### 3.7 `includes/ocr/plate_decode.h` — CTC 디코딩 + 문법 (110줄)

PC 실험실(ocr_lab)에서 검증한 로직의 C++ 이식:

- `ArgmaxPerStep`: 출력이 logits 면 그 스텝만 softmax (값 범위로 자동 판별)
- `CtcCollapse`: 연속 중복 제거 + blank 제거 → 문자열
- `Confidence`: 비블랭크 스텝 확률의 **기하평균** — 로그의 `conf` 가 이 값
- `ValidPlateFormat`: 바이트 파싱 문법 검사(정규식 없음)
  - 일반판: 숫자 2~3 + 한글 1 + 숫자 4 (`12가3456`, `123가4568`)
  - **지역판**: 한글 2(지역명) + 숫자 1~3 + 한글 1 + 숫자 4 (`서울12가3456`)
  - 이 문법이 1차 환각 필터 — 무효 텍스트는 어느 관문에도 못 들어감

### 3.8 `includes/ocr/plate_ocr.h` — 판독 엔진 (320줄) ★심장부

`Recognize(crop, light)` 한 함수가 판독 파이프라인 전체:

1. **색 분류** `ClassifyPlateColor`: 밝은 픽셀(판 배경) 평균색 → wh/ye/gr/bl.
   `ColorConsistent`: 영업용 한글(아바사자배)은 노랑판 전용 — 흰판의 "27아…" 환각 즉시 기각
2. **멀티후보 토너먼트** `MakeCandidates`: WiseAI 크롭은 여백이 헐렁해서 원본만 읽으면
   17% 헛읽음(실측) → 한 크롭을 여러 방식으로 재프레이밍해 전부 읽고 최고를 채택
   - full: `orig` + `box0m0`(Otsu 밝은영역 타이트 크롭) + `ctr0`(중앙 70×45%) + `ctr1`(85×60%) = 4개
   - light(버스트): 3개. 8개→4개 다이어트는 우승 통계(win= 계측)로 무용 후보를 처형한 결과
   - 선택 규칙: **유효(문법+색) > 신뢰도**
3. **지터 앙상블** (conf<0.95 일 때만): 이긴 크롭을 ±3px/스케일 재프레이밍 재판독.
   레터박스 정렬 2px 차이로 7↔8, 라↔파가 갈리는 성질 이용. **결과는 0.94 캡**
   — "재프레이밍이라 무해" 가설이 오답 부스트 사고로 반증된 뒤의 교훈
4. **조도 정규화** (conf<0.95 & lum 이상일 때): 감마 LUT 로 평균 128 맞춰 재판독(~1ms).
   로그의 `lum 83>128` 이 이것. 결과 0.94 캡
5. **구조대** (`kRescueChain=false` 로 봉인): 샤프닝/디노이즈/채널 라우팅 — 2코어 예산 절약

> 설계 헌법: **"선택 이후 conf 를 올리는 모든 장치는 캡"** — 같은 사고(가공본이 오답을
> 게이트 위로 부스트)가 4번 반복되고 확립된 규칙.

### 3.9 `includes/ocr/plate_vote.h` — 챔피언십 투표 (156줄)

차(oid)별 투표함. 굿샷(primary) + 버스트 표를 모아 트랙 종료 시 개표.

- `CanSample`: 버스트 예산 관리 (웜업 1s / 스로틀 150ms / 상한 3)
- `HasConflict`: 즉시확정 발사 전 "다른 텍스트 0.98+ 반박" 검사 — 0.99 오독의 폭주 방지
- `HasPrimary`: 굿샷 표 존재 여부 — 없으면 만료 유예 ×3
- `Finalize` — 규칙이 전부 실측 사고의 흉터다:
  - 그룹 점수 = `max(conf) + 0.02×min(표수-1,3) + primary 0.02`
    (순수 conf 만 쓰면 오답 0.99 한 방이 정답 ×4를 이김 — 실측)
  - 동점은 max_conf 우선 (primary 우선이 버스트 정답 1.00 을 눌러버린 사건)
  - **한글 접전 캡**: 숫자 동일 + 한글만 다른 경쟁이 0.03 이내 → 0.94 (tinyLPR 최대 약점이
    한글 음절 — 오확정 전원이 이 패턴이었음). 0.99+ 우승자와 교차합의는 면제
  - 교차합의(굿샷+버스트 동일 텍스트 2표+) → 0.97 승격 / 버스트 단독 max<0.98 → 0.94 억제

### 3.10 `includes/ocr/plate_db.h` — 등록차량 DB 그물 (110줄)

실전 "아파트 등록차량 목록"에 해당하는 정당한 레이어. **코드는 정답을 모른다** —
목록은 외부 파일(`registered_plates.txt`, 한 줄 1판, # 주석 허용)에서 로드, 없으면 자동 오프.

- UTF-8 → 코드포인트 배열(한글 1글자=1요소) 후 **편집거리 ≤1**(치환/삽입/삭제) 비교
- `Match` = 0(없음)/1(유일)/2(복수→불개입). 정확 일치는 즉시 유일 확정
- **지역명 스트립**: 원문 불일치 & 선두가 한글2+숫자 꼴이면 지역명 떼고 재대조
  (`경기36라7833` 환각 회수). 진짜 지역판 등록차는 지역명 포함으로 넣으면 원문에서 먼저 걸림
- 쓰임: FINAL 교정(db-fix), 게이트 미달 회수(db-rescue exact≥0.80 / ed1≥0.85)

### 3.11 `includes/io/` — 이벤트 출구 (sink 패턴)

- `i_event_sink.h`: `OnEvent(ch, msg)` 인터페이스 하나
- `debug_viewer_sink.h`: SDK 원격 디버그 메시지로 전송 (PC 의 remote_debug_viewer 가 수신 — 네가 보는 그 로그)
- `disk_log_sink.h`: `../storage/events.log` 누적(비대해지면 트렁케이트) → `/eventlog` 로 조회

도메인 코드는 `EmitEvent()` 한 줄만 부르고, 목적지는 sink 목록이 결정 — 출구 추가가 한 줄이다
(4단계 리팩터링의 확장성 증명).

### 3.12 기타

- `i_sample_component.h`: 컴포넌트 ID 선언 (SDK 규약)
- `CMakeLists.txt`: OpenCV 정적 링크 + tflite + `ocr_models/`(모델·라벨·**registered_plates.txt**) 를 res 로 패키징
- `ocr_models/`: `kr_lpr.tflite`(87KB) + `kr_lpr_labels.txt`(문자셋) + `registered_plates.txt`
  (multiline.tflite 은 기여 0 실측으로 미사용)

---

## 4. 판정 상태기계 — 문턱값 전부

```
크롭 판독(conf) ──┬─ ≥0.985 & 무반박(0.98+) ──────────────► ★FINAL (instant, db-fix 가능)
                  └─ 그 외 → 투표함
트랙 만료(15f/3s, primary 없으면 ×3) → 개표:
  챔피언 conf (접전캡 0.94 / 교차합의 0.97 / 버스트단독 억제 적용 후)
      ├─ ≥0.95 ───────────────► ★FINAL  (미등록+유일 ed1 매칭이면 db-fix 교정)
      └─ <0.95 ─┬─ DB 유일 매칭 & (정확≥0.80 | ed1≥0.85) ─► ★FINAL (db-rescue[-fix])
                └─ 그 외 ──────► ?HOLD  (발급 거부 — 로그만)
```

봉인된 실패들(다시 시도 금지, 전부 실측 낙제):
- TFLite 2스레드 → 스핀 동결. 속도는 "일 줄이기"로만
- 지터/노출/스태킹 결과 무캡 승격 → 오답이 게이트 뚫음 (4회 반복 사고)
- 후보 캐스케이드 조기 종료(0.99에서 중단) → **0.99짜리 오독 실존**(27하8257 0.99 등),
  나머지 후보의 1.00 정답이 뒤집을 기회를 없애 야간 성능 하락. 재시도하려면
  "1.00 만 조기 종료" 또는 "두 후보 합의 시만"으로
- 버스트 스냅샷 크롭 conf<0.90 은 환각 (시간 어긋남) — 표로 안 받음

---

## 5. HTTP API (이더넷 연동)

모든 엔드포인트는 카메라 웹서버를 통해 노출된다 (digest 인증 = 카메라 계정):

```
http://<카메라IP>/opensdk/object_detect/<path>
예) curl --digest -u admin:*** "http://192.168.0.5/opensdk/object_detect/detections?ch=2"
```

| Path | 응답 | 용도 |
|---|---|---|
| `/detections?ch=N` | JSON: 움직이는 객체+번호판 박스 | **외부 시스템 폴링용 메인** (moving/plate/box) |
| `/platetext?ch=N` | JSON: 마지막 OCR 결과 + **`final`(마지막 확정 번호)** | 차단기 연동에 바로 쓸 수 있는 값 |
| `/lastplate[?id=]` | JPEG | 마지막(또는 특정 차) 번호판 크롭 |
| `/snapshot?ch=N` | JPEG | 라이브 프레임 |
| `/config` (GET/POST) | `{"person":bool,"vehicle":bool}` | 감지 on/off 원격 제어 |
| `/platelist`, `/plate?n=` | JSON / JPEG | 저장 갤러리(검증) |
| `/eventlog` | text | DiskLogSink 이력 |
| `/rawmeta`, `/rawevents`, `/imgref`, `/lsdownload`, `/sysinfo`, `/candlist`, `/cand` | — | 진단/탐사용 |

아웃바운드(앱→외부 서버 push)도 가능(소켓 자유) — 현재 미구현, sink 하나 추가하면 됨
(예: `HttpPostSink` 로 FINAL 발생 시 POST).

---

## 6. 로그 문법 요약

```
plate detected (ch2 id59) imgref=X          감지(트랙 탄생). O=지연 재저장
  burst ch2 id59 sample#1 "..." (conf, box, ms)   버스트 표
plate SAVE  #slot (total N) ch2 oid59       굿샷 크롭 저장
OCR start ... crop=WxH sharp S (load ms)    판독 시작 (sharp=라플라시안 분산)
  tinyLPR(4 cands): "..." | win ctr0 | lum L | color wh | rescue ms    토너먼트 결과
  OCR result ... (conf, by tiny/jitter/exposure, total ms)             보정 경로 포함 확정
  instant hold ...: 버스트 반박 존재 -> 투표행
★ PLATE FINAL ... (instant | best-of-N, conf, src=good-shot|burst[, db-fix|db-rescue[-fix]])
? PLATE HOLD  ... (발급 거부)
OCR skipped ...: crop too small (WxH < 300x176)   크기 검문
```

상세 해부(필드별 색인)는 별도 아티팩트 "번호판 판독 파이프라인 해부" 참고.

---

## 7. 왜 이 구조인가 (설계 원칙)

1. **카메라가 잘하는 건 카메라에게** — 감지/추적/베스트샷은 WiseAI(NPU). 앱은 판독과 판정만.
2. **오독은 전제, 오확정은 금지** — 모델 천장(88%)을 판정 구조로 넘는다. 확신 없으면 침묵(HOLD).
3. **모든 문턱값은 실측에서** — config.h 의 숫자마다 사건번호(어떤 오확정이 낳았는지)가 주석에 있다.
4. **가공은 캡** — 원본 판독만이 게이트를 단독으로 넘을 자격이 있다.
5. **이식성** — XML 파싱(parser)/저장(store)/판독(ocr)/판정(vote/db)/출구(sink)가 서로를 모른다.
   카메라를 바꾸면 parser 만, 출구를 늘리면 sink 만 만진다.
```
