# edge-lpr — 카메라 위에서 완결되는 한국 번호판·전기차 인식

한화비전 **PNM-C16083RVQ** 멀티디렉셔널 카메라에서 도는 Wisenet Open Platform(OpenSDK) 엣지 앱.
서버·GPU 없이 **카메라 안에서** 번호판 검출 → OCR → 3단 판정 → 확정까지 끝내고,
나아가 그 차가 **전기차인지까지 스스로 판정**한다.

- **검출**: WiseAI 번호판 박스 + 카메라가 미리 잘라주는 good-shot 크롭(`ImageRef`)
- **인식**: [tinyLPR](https://github.com/noahzhy/KR_LPR) 87KB TFLite 모델 (CTC, 192×96 gray)
- **확정**: 멀티후보 판독 + 신뢰도 챔피언십 + 등록차 DB 그물 — **"틀리게 확정하느니 침묵한다"**
- **전기차**: 등록차는 DB 플래그, 미등록차는 카메라가 [ev.or.kr](https://ev.or.kr) 실조회 (`/isev` API)
- **제약**: 앱에 CPU 2코어만 허용, SDK 26.05.19 / SoC cv5

## 3단 판정 (오확정 0의 비결)

| 단계 | 조건 | 결과 |
|---|---|---|
| 1단 · 즉시확정 | good-shot conf ≥ 0.985 & 문법 유효 & 버스트 무반박 | ★FINAL (즉시) |
| 2단 · 챔피언십 투표 | good-shot + 버스트 표 개표, 게이트 ≥ 0.95 | ★FINAL |
| 3단 · 등록차 DB 그물 | 위 미달이라도 등록명부와 1글자 이내 유일 매칭 | ★FINAL (회수/교정) |
| 전부 미달 | | ?HOLD (침묵) |

```
🎨 COLOR ch2 "15주5957" -> blue (bl)
★ PLATE FINAL ch2 id300 -> "15주5957" (instant, conf 1.00, src=good-shot)
⚡EV ch2 "15주5957" -> ★EV★ (ev.or.kr lookup: BMW iX3, 전기, 1종)
```

## 전기차 판정 (`/isev`)

번호판을 키로 던지면 카메라가 등록·전기차·목격 여부를 JSON으로 답한다.

```bash
curl --digest -u admin:*** "http://<카메라IP>/opensdk/object_detect/isev?plate=42주0120"
# {"plate":"42주0120","registered":true,"ev":false,"seen":true,"color":"wh","age_ms":12000}
```

- **등록차**: `registered_plates.txt` 의 `,ev` 플래그 (등록 시점에 `tools/ev_lookup.py` 로 확정)
- **미등록차**: 카메라가 내장 curl 자식 프로세스로 ev.or.kr 실조회 (앱 내부 TLS는 즉사 → popen 방식)
- 판 색은 EV 판정에 쓰지 않는다 (법인 전기차는 연두색이라 "파랑=EV"가 틀림)

## 빌드 / 배포

```bash
SDK_VER=26.05.19 SOC=cv5 APP_NAME=object_detect docker compose up   # → object_detect.cap
```

카메라 웹 UI → 설정 → 오픈플랫폼 → `.cap` 업로드 → Start.
**설치는 영상 정지 + 카메라 재부팅 직후** (임시공간 고갈 시 패키지 오류).
앱 ID는 `object_detect` 유지 (레포 이름과 다름 — 패키징 제약).

- 라이브 확인: `http://<카메라IP>/opensdk/object_detect/` 웹 UI
- 판독 API: `GET /platetext?ch=N` · 전기차 API: `GET /isev?plate=...`
- 실시간 로그: `CLI/remote_debug_viewer` (PC 실행, DebugHelper 앱 필요)

## 문서

| 문서 | 내용 |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | **동작 원리 전해부** — 전체 흐름·모듈별 역할·판정 상태기계·HTTP API |
| [config/CAMERA_SETTINGS.md](config/CAMERA_SETTINGS.md) | 확정된 카메라 골든 파라미터 (셔터·노출·역광보정 등) |

## 도구

- `tools/ev_lookup.py` — 번호판 → ev.or.kr 전기차 조회 CLI (`--update` 로 명부에 `,ev` 자동 기록)
