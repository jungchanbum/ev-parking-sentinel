# 라즈베리파이 연동 가이드 — EV 주차위반 감지

> 카메라 담당이 라파 담당에게. 프로토콜 상세는 `PARKING_EVENTS.md` 참고.
> 이 문서만 보고 바로 구현 가능하게 쓴 실무 가이드다.

## 1. 니가 받게 되는 것 (요약)

카메라(192.168.0.5)가 주차구역을 감시하다가, **이벤트는 딱 한 종류**로 알린다:

```
Channel.0.OpenSDK.object_detect.ParkingOccupied=True      ← 채널에 주차 있음
Channel.0.OpenSDK.object_detect.ParkingOccupied.2=True    ← 2번 칸에 주차됨
```

1. **차가 주차되면** → `Occupied.칸번호=True` 푸시
2. **번호판·전기차 여부가 판정되면** → 같은 라인이 펄스로 깜빡임 (재열람 신호)
3. **차가 구역에서 90% 나가면** → `Occupied.칸번호=False` **즉시** 푸시
4. **위반인지 아닌지**는 이벤트가 아니라 **XML의 `violation` 필드**로 판단한다
   (이벤트 상황판은 펌웨어 제약상 불리언만 실려서 상세는 XML 담당)

상세 데이터(번호판, 전기차 여부, 위반, 주차시각, 증거사진 URL)는
이벤트 받을 때마다 XML 한 번 읽으면 전부 나온다.

## 2. 네트워크 요구사항

- 카메라와 같은 LAN (`192.168.0.x`)
- 카메라로 나가는 HTTP(80) 아웃바운드만 있으면 됨 — **라파 쪽 포트 개방 불필요**
  (모든 통신이 라파→카메라 방향. 카메라가 라파로 접속하는 건 없음)
- 인증: HTTP Digest, 계정은 카메라 담당에게 받을 것

## 3. 구현 규칙 (이 세 줄이 전부다)

```
규칙 1: monitordiff 연결을 하나 열어두고 유지한다 (읽기 타임아웃 X).
규칙 2: "ParkingOccupied" 라인이 오면 (값 무관) → XML을 읽는다.
규칙 3: XML의 칸별 상태로 직전과 비교해 이벤트 처리한다.
        (주차 = occupied false→true / 갱신 = 내용 변화 / 출차 = true→false)
```

- **알람(GPIO)은 XML의 `<violation>true</violation>` 기준** (칸 단위로 정확)
- `ev=unknown`(판정중)은 위반 아님 — 알람 올리지 말 것
- 연결이 끊기면 3초 후 재접속 — 재접속 첫 스냅샷으로 상태 자동 복구
- 칸번호 `.N` = XML의 N번째 `<space>` (등장 순서)

## 4. 레퍼런스 구현 (검증 완료)

`tools/parking_watch.py` — 위 규칙 그대로 구현된 파이썬 워처.
2026-08-04 모형차 리그에서 주차/갱신/출차 전 사이클 실측 통과.

```
[16:29:21] 🚗 주차   칸=ch0-01 ch0 | 번호=49허5678 | 주차시각=16:29:21 | 내연차 (registered) | ⛔위반(비EV)
           증거사진: http://192.168.0.5/opensdk/object_detect/plate?n=0
[16:29:51] 🏳️ 출차   칸=ch0-01 | 나간 차=49허5678
```

실행: `python3 parking_watch.py` (의존성: `requests` 뿐)

### GPIO 붙이는 위치

`diff_and_print()` 안의 세 분기가 이벤트 핸들러다:

```python
if d["occupied"] and not was:          # 🚗 주차 발생
    if d["violation"]:
        GPIO.output(ALARM_PIN, GPIO.HIGH)      # ← 여기
elif d["occupied"] and was and sig(p) != sig(d):   # 🔄 판정 갱신
    GPIO.output(ALARM_PIN, GPIO.HIGH if d["violation"] else GPIO.LOW)  # ← 여기
elif not d["occupied"] and was:        # 🏳️ 출차
    pass                               # 아래 한 줄 방식이면 여기선 할 일 없음
```

가장 단순한 방식 (권장): 매 `diff_and_print()` 끝에 한 줄 —

```python
GPIO.output(ALARM_PIN, GPIO.HIGH if any(x["violation"] for x in cur.values()) else GPIO.LOW)
```

칸이 몇 개든 "위반이 하나라도 있으면 사이렌"으로 상태가 항상 동기화된다.

## 5. 손으로 테스트 (개발 전 확인용)

```bash
# ① 현재 상태 스냅샷 — ParkingOccupied 줄이 보이는지
curl -s --digest -u <ID>:'<PW>' \
  "http://192.168.0.5/stw-cgi/eventstatus.cgi?msubmenu=eventstatus&action=check" | grep object_detect

# ② 라이브 — 차가 들어오고 나갈 때 라인이 떨어지는지
curl -s -N --digest -u <ID>:'<PW>' \
  "http://192.168.0.5/stw-cgi/eventstatus.cgi?msubmenu=eventstatus&action=monitordiff" \
  | grep --line-buffered object_detect

# ③ 상세 XML — 번호판·EV·위반·증거
curl -s --digest -u <ID>:'<PW>' "http://192.168.0.5/opensdk/object_detect/parking_status"
```

## 6. XML 필드 치트시트

| 필드 | 값 | 의미 |
|---|---|---|
| `space@id` | `ch0-01` | 칸 고유번호 (채널-순번) |
| `occupied` | true/false | 주차 여부 |
| `parked_ms_ago` | ms / -1 | 주차 후 경과 (주차시각 = now − 이 값) |
| `plate` | `49허5678` / 빈값 | 번호판 (빈값 = 판독중) |
| `ev` | yes/no/unknown | 전기차 여부 (`unknown` = 판정중) |
| `ev@source` | registered/lookup | 판정 출처 (등록DB / ev.or.kr 실조회) |
| `violation` | true/false | **이 칸이 단속 대상인가** (비EV 주차) — GPIO 기준 |
| `evidence` | URL 경로 | 증거 크롭 사진 (호스트 붙여서 GET) |

## 7. 트러블슈팅

| 증상 | 원인/조치 |
|---|---|
| check 에 object_detect 줄이 없음 | 앱이 안 돌고 있음 — 카메라 담당 호출 |
| monitordiff 가 즉시 끊김 | Digest 인증 실패 — 계정 확인 |
| 주차했는데 plate 가 계속 빈값 | 판독 재시도 중 (수 초~수십 초) — `ev=unknown` 인 동안 알람 금지 |
| 라인이 한동안 안 옴 | 정상 (변화 없으면 침묵) — 불안하면 10초 안전망 폴링 병행 (레퍼런스 구현에 포함) |
| 카메라 재부팅 | 재접속 루프가 자동 복구. 구역 설정도 카메라에 영속화돼 있어 재등록 불필요 |
| `ParkingViolation` 이라는 옛 줄이 보임 | 구버전 잔재 — 무시. 현행 이벤트는 `ParkingOccupied` 하나다 |
