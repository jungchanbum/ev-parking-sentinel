# edge-parking-lpr — 카메라 안에서 완결되는 주차 관제 (한국 번호판 · EV 위반)

한화비전 **PNM-C16083RVQ** 멀티디렉셔널 카메라에서 도는 Wisenet Open Platform(OpenSDK) 엣지 앱.
서버·GPU 없이 **카메라 안에서** 번호판 인식 → 주차 판정 → 전기차 판정 → 위반 판단까지 끝내고,
외부 장치(Raspberry Pi, Qt 앱)는 판단 로직 없이 결과만 받아 쓴다 — **엣지가 완결된 판정기**.

```
차량 진입 ─ WiseAI 감지 ─ 앱: 구역 판정(65%+2s) ─ OCR 합의 ─ EV 판정 ─ 위반 판단
                                                                       │
                     Pi / Qt  ◀── ① 이벤트 벨 (SUNAPI eventstatus 푸시) ─┤
                              ◀── ② 상태 XML (REST 풀) ─────────────────┘
```

- **검출**: WiseAI 번호판/차량 메타데이터 (정지차 드랍·id 재사용까지 실측으로 규명하고 흡수)
- **인식**: [tinyLPR](https://github.com/noahzhy/KR_LPR) 87KB TFLite (CTC, 192×96) — 온보드 추론, 2코어 제약
- **판독 소스**: 버스트(라이브 bbox + 풀해상 스냅샷 자체 크롭, 주력) + good-shot(WiseAI 크롭, 보조)
- **원칙**: **"틀리게 확정하느니 침묵한다"** — 모든 파라미터에 실측 근거 주석
- **제약**: 앱에 CPU 2코어, SDK 26.05.19 / SoC cv5, 앱 내부 TLS 금지(→ popen curl)

## 번호판 판독 — 3단 판정 (오확정 0의 비결)

| 단계 | 조건 | 결과 |
|---|---|---|
| 1단 · 즉시확정 | good-shot 원본 conf ≥ 0.985 & 문법 유효 & 무반박 | ★FINAL (즉시) |
| 2단 · 챔피언십 투표 | 버스트+good-shot 2표 합의, 게이트 ≥ 0.95 (가공 판독은 캡 0.94 — 단독 확정 불가) | ★FINAL |
| 3단 · 등록차 DB 그물 | 등록명부와 1글자(ed≤1) / 같은 길이 2글자 유일 매칭 | ★FINAL (회수/교정) |
| 전부 미달 | | HOLD (침묵) |

- 판독은 **주차구역 근처(칸 bbox+35%) 번호판만** — 화면 반대편(모니터·옆 차선) 오배정 원천 차단
- 미등록 판독은 빈 칸만 채우고 기존 확정 번호를 **교체할 수 없음** (저품질 크롭 오염 방지)
- **베스트 프레임 모드**(`best_frame_mode=1`, 옵션): 진입~주차 동안 버스트 크롭의 sharp×크기 챔피언
  1장만 주차 확정 순간 OCR. 채널당 1대 전제 — **여러 대 동시**엔 0(차별 투표)으로 둘 것
- **쓰레기 크롭 필터**: 선명도(`best_min_sharp`)·가로비율(`best_min_aspect`) 미달 크롭은 최종 OCR 제외
  (블러·로고 오크롭이 환각 번호를 뱉는 것 차단 — 문턱은 실측 분포 기준으로 패널에서 조정)

### ★ 모형(토이카) 리그의 제1 조건 — 번호판 폰트

**모형 판은 반드시 실판 글리프(각진 전용 서체)로 인쇄해야 한다** (2026-08-11 확정 실측).
tinyLPR 는 실제 한국 번호판 폰트로만 학습돼, 둥근 범용 폰트 인쇄물은 sharp 2000+ 로 선명해도
체계적으로 오독한다(9→0, 서→거). 반대로 실판 글리프는 강블러·저해상·원근왜곡에도 정독
(같은 판을 폰트만 바꿔 conf 0.80 오독 → **conf 1.00 정독** 실측). 초점·크롭·후처리보다 폰트가 먼저다.
인쇄 시트 생성: `ocr_lab` 의 `make_plate_sheet.py` (실판 글리프·실판 비율·색상별).

## 주차 · 위반 판정 (상태기계)

```
후보 (양방향 겹침 65% + 정지 2초) ──▶ PARKED (앱이 점유를 기억 — WiseAI 침묵과 무관)
   ▲                                     │ 번호 확정 + EV 판정 → 위반 = 점유 ∧ 비EV
   │                                     ▼
대기 ◀── 리셋 + 유령 id 15초 차단 ◀── LEFT (90% 이탈 즉시 / 육안검증 3연속 빈칸)
```

- **점유 유지**: 칸을 덮는 "정지" 차량이 있으면 id 무관 유지 (WiseAI id 뺑뺑이 흡수)
- **출차 육안검증**: 부재 의심 시 스냅샷을 정조준 크롭해 "그 칸의 번호와 같은 판"이 보이는지 확인
  — 환각(매번 다른 번호)과 진짜 판(항상 비슷한 번호)을 **일관성으로 구분** (실측 기반)
- 출차 순간 그 차의 연산 전량 폐기 + 나간 차의 유령 번호판 레코드만 차단 (새 차는 0초 대기)

## 외부 연동 — notify-then-fetch

이벤트 채널은 불리언만 실을 수 있고, 폴링 전용은 반응-부하 트레이드오프가 생긴다. 그래서 2채널:

| 채널 | 프로토콜 | 내용 |
|---|---|---|
| 벨 (푸시) | SUNAPI `eventstatus.cgi` monitordiff 스트리밍 | 커스텀 이벤트 `ParkingOccupied` — 채널 + 칸별 불리언. 내용 변화는 False→True 펄스로 diff 강제 |
| 서류철 (풀) | REST `GET /parking_status` (XML) | 칸별 점유/번호/EV/위반/증거 URL/구역 좌표 |

클라이언트 규칙은 3줄: *"Occupied 라인이 변하면 XML을 읽고 violation 필드로 판단하라."*
레퍼런스 구현: [tools/parking_watch.py](tools/parking_watch.py) (PC/Pi 겸용 워처).

## 전기차 판정

- **모든 차량이 ev.or.kr 실조회** — 등록차도 예외 없음 (카메라가 popen curl 로 직접 조회;
  앱 내 TLS 는 즉사라 자식 프로세스 우회). 명부의 `,ev` 플래그는 **조회 실패 시 폴백**일 뿐
- 명부(`registered_plates.txt`)의 역할은 **번호 교정(오독 회수)** — EV 판정의 출처가 아니다
- 판 색으로 판정하지 않는다 — 법인 전기차는 연두판이라 "파랑=EV" 규칙은 틀림

```bash
curl --digest -u admin:*** "http://<IP>/opensdk/object_detect/isev?plate=42주0120"
# {"plate":"42주0120","registered":true,"ev":false,"seen":true,...}
```

## 런타임 튜닝 — 빌드 없이 파라미터 실험

주차 판정 파라미터 12종(겹침 문턱·확정 대기·부재 유예·이탈 문턱·육안검증 횟수/주기/conf·버스트 사거리·
굿샷 우선·베스트프레임 on/off·최소 선명도·최소 가로비율)을 `GET/POST /parking_tune` 으로 즉시 조정 —
부분 갱신·범위 검증·영속화(재설치에도 유지). 앱 웹 UI의 **⚙️ 판정 튜닝 패널**에서도 조작 가능.
튜닝 사이클: 재빌드+설치 10분 → **3초**.

## 빌드 / 배포

```bash
SDK_VER=26.05.19 SOC=cv5 APP_NAME=object_detect docker compose up   # → object_detect.cap
```

- 설치: 카메라 웹 UI → 오픈플랫폼 → `.cap` 업로드 → Start
- **수칙: 영상 정지 + 카메라 재부팅 직후 설치** — 연속 설치 시 조용한 반쯤-설치 오염 (Status=Running인데 HTTP 전부 500; 물리 재부팅으로만 회복)
- 앱 ID는 `object_detect` 유지 (레포 이름과 별개 — 카메라 설치 식별자)
- 카메라 화질·AI 설정 골든 파라미터: [config/CAMERA_SETTINGS.md](config/CAMERA_SETTINGS.md) (복원 명령 포함)

## 디버깅 — 실시간 이벤트 보기

| 방법 | 비고 |
|---|---|
| `CLI/remote_debug_viewer/app/bin/remote_debug_viewer object_detect` | SDK 원격 뷰어. **앱 스켈레톤 포트 고정(8590)이 전제** — `app/src/PLifeCycleManagermanifest.json` 의 `SkeletonPortNumber: 8590` 과 뷰어 설정의 object_detect PortNumber 가 일치해야 연결. `auto` 면 뷰어가 포트를 몰라 `port # 0` 실패 |
| `GET /eventlog` | 같은 이벤트를 HTTP 로 — 포트 설정 불필요, 항상 동작 |
| `CLI/watch_events.sh` | `/eventlog` 를 1초 tail + 판독/FINAL/위반 색 강조 (WSL) |

주의: 매니페스트는 빌드가 **`app/src/`(원본) → `app/bin/`** 으로 재생성한다.
포트 등 매니페스트 수정은 반드시 `app/src/` 쪽에 — `app/bin/` 만 고치면 다음 빌드가 되돌린다.

## HTTP API (주요)

| 경로 | 역할 |
|---|---|
| `GET /parking_status` | 칸별 실시간 상태 XML (좌표 포함) — 외부 연동의 서류철 |
| `GET/POST /parking_roi` | 구역 등록/삭제 (4점 폴리곤, `?ch=`) |
| `GET/POST /parking_tune` | 판정 파라미터 런타임 튜닝 |
| `GET /isev?plate=` | 등록·전기차·목격 여부 JSON |
| `GET /snapshot?ch=` `GET /detections?ch=` | 라이브 뷰 / 감지 박스 |
| `GET /plate?n=` `GET /platelist` | 판독 크롭 갤러리 (검증용) |

전체 목록·저작 가이드: [docs/APP_HTTP_API.md](docs/APP_HTTP_API.md)

## 문서

| 문서 | 내용 |
|---|---|
| [docs/LOGIC_TOUR.html](docs/LOGIC_TOUR.html) | **동작 원리 투어 (다이어그램 6개)** — 파이프라인·크롭 2경로·OCR 내부·3단 판정·상태기계·EV·외부 연동 + 실측 교훈. [PDF 판](docs/LOGIC_TOUR.pdf) |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | 동작 원리 전해부 — 모듈·판정 상태기계·데이터 흐름 |
| [docs/PARKING_EVENTS.md](docs/PARKING_EVENTS.md) | 외부 연동 통신 명세 (벨+서류철) |
| [docs/PI_INTEGRATION.md](docs/PI_INTEGRATION.md) | Raspberry Pi 엔지니어 작업 가이드 (GPIO 3줄 규칙) |
| [docs/APP_HTTP_API.md](docs/APP_HTTP_API.md) | OpenSDK 앱 HTTP API 만드는 법 (실전 예제 해부) |
| [config/CAMERA_SETTINGS.md](config/CAMERA_SETTINGS.md) | 카메라 ISP 파라미터·WiseAI 분석 설정 골든 값 (실측 근거) |

## 도구

- `tools/parking_watch.py` — 이벤트 벨 수신 → XML 조회 → 주차/갱신/출차 콘솔 워처 (Pi 레퍼런스)
- `tools/ev_lookup.py` — 번호판 → ev.or.kr 전기차 조회 CLI (`--update` 로 명부 자동 기록)
