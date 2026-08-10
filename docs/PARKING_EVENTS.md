# 주차 이벤트 통신 명세 (카메라 ↔ 외부 시스템)

> 대상 장비: Hanwha PNM-C16083RVQ + `object_detect` 앱 (OpenSDK)
> 검증일: 2026-08-04 (모형차 리그 실측)

## 0. 한눈에 보는 구조

```
┌─────────────── 카메라 (192.168.0.5) ───────────────┐
│  object_detect 앱                                   │
│   주차판정 → OCR → EV판정 → 위반판정                │
│      │                                              │
│      ├─ [벨]   ParkingOccupied ──→ monitordiff 푸시 │──→ 라파/PC
│      └─ [서류] /parking_status XML ←─ HTTP GET ─────│←── 라파/PC
└─────────────────────────────────────────────────────┘
```

- **벨 (실시간 푸시)**: `ParkingOccupied` 하나 — "칸에 일이 생겼다"는 불리언 신호.
  (EventStatus 상황판은 펌웨어 제약으로 **불리언만** 실을 수 있다 — 번호판 같은
  문자열·상세는 원리적으로 못 실림)
- **서류철 (요청형 XML)**: 칸별 상세 전부 — 번호판, EV, **위반 여부**, 시각, 증거사진.
- 기본 사용 패턴: **벨이 울리면 서류철을 읽는다.** 위반 판단도 XML 의
  `violation` 필드로 한다 (벨은 트리거일 뿐).

## 1. 벨 — ParkingOccupied (SUNAPI eventstatus.cgi)

### 1.1 상황판 라인 포맷

```
Channel.<ch>.OpenSDK.object_detect.ParkingOccupied=<True|False>      ← 채널 종합
Channel.<ch>.OpenSDK.object_detect.ParkingOccupied.<N>=<True|False>  ← 칸별
```

- `<N>` = **칸 순번** — XML(`/parking_status`)의 그 채널 `<space>` 등장 순서와 동일.
  `.1` = 첫 번째 space, `.2` = 두 번째 space, …

### 1.2 라인이 나가는 순간

| 상황 | 라인 |
|---|---|
| 칸에 주차 확정 (정지 3초) | `Occupied.<N>=True` |
| 칸에서 출차 (구역 90% 이탈 시 **즉시**) | `Occupied.<N>=False` |
| 점유 중 내용 갱신 (번호 확정·EV 판정) | `Occupied.<N>` **펄스** (False→True 연속 2줄) |

**수신 규칙은 하나다: "라인이 오면 (값 무관) XML 을 읽는다."**
펄스는 값이 안 변한 갱신에도 diff 라인을 강제로 만들기 위한 장치라,
값을 해석할 필요 없이 라인 수신 자체를 트리거로 쓰면 모든 경우가 커버된다.

### 1.3 접속 방법

```bash
# 스냅샷 (현재 상태 전부, 1회)
curl -s --digest -u <ID>:'<PW>' \
  "http://192.168.0.5/stw-cgi/eventstatus.cgi?msubmenu=eventstatus&action=check"

# 라이브 (접속 유지 — 첫 응답=스냅샷, 이후 변화분만 multipart 로 푸시)
curl -s -N --digest -u <ID>:'<PW>' \
  "http://192.168.0.5/stw-cgi/eventstatus.cgi?msubmenu=eventstatus&action=monitordiff"
```

- 인증: HTTP **Digest**
- monitordiff 는 변화가 없으면 몇 분이고 침묵 → **읽기 타임아웃을 걸지 말 것**
  (또는 타임아웃을 안전망 폴링으로 활용 — `PI_INTEGRATION.md` 참고)
- 연결이 끊기면 재접속 — 재접속 첫 스냅샷이 현재 상태라 놓친 이벤트가 자동 복구됨

## 2. 서류철 — /parking_status (XML)

```bash
curl -s --digest -u <ID>:'<PW>' "http://192.168.0.5/opensdk/object_detect/parking_status"
```

```xml
<parking>
  <space id="ch0-01" channel="0">          <!-- 칸 고유번호 / 채널 -->
    <occupied>true</occupied>              <!-- 점유 여부 -->
    <parked_ms_ago>487043</parked_ms_ago>  <!-- 주차 후 경과(ms), 빈칸=-1 -->
    <plate>96머5715</plate>                <!-- 번호판 (판독 전이면 빈값) -->
    <ev source="registered">yes</ev>       <!-- yes/no/unknown + 출처 -->
    <violation>false</violation>           <!-- ★ 이 칸의 위반 여부 (알람 기준) -->
    <evidence>/opensdk/object_detect/plate?n=1</evidence>  <!-- 증거 크롭 URL -->
    <polygon><pt x="0.49" y="0.63"/>…</polygon>            <!-- 구역 4점 (정규화) -->
  </space>
  <!-- 칸 수만큼 <space> 반복 — 문서는 항상 한 장, 전 채널 전 칸 포함 -->
</parking>
```

- `violation=true` = 비EV 차량의 주차 = **단속 대상** (GPIO 알람 기준)
- `ev@source`: `registered`(등록차량 DB) | `lookup`(ev.or.kr 실조회) | 빈값(미판정)
- `ev=unknown` 은 판정 대기 — **위반으로 취급하지 않는다** (억울한 알람 방지)
- `evidence` 는 카메라 호스트를 앞에 붙여 사용: `http://192.168.0.5` + evidence
- 파일이 아니라 요청 순간 즉석 생성 — 항상 최신, 아무리 자주 읽어도 무부하

## 3. 구역 관리 — /parking_roi

```bash
# 등록 (본문 = 4점 8숫자, 정규화 0~1; Pi 표시좌표는 기본 좌우반전 저장)
curl -s --digest -u <ID>:'<PW>' -X POST -d "[0.49,0.63, 0.71,0.62, 0.71,0.87, 0.49,0.87]" \
  "http://192.168.0.5/opensdk/object_detect/parking_roi?ch=0"        # → {"id":"ch0-01"}

curl … "…/parking_roi?ch=0"              # GET: 그 채널 구역 목록 XML
curl … -X POST "…/parking_roi?ch=0&delete=ch0-01"   # 하나 삭제
curl … -X POST "…/parking_roi?ch=0&clear=1"         # 채널 전체 비움
```

- 구역은 카메라에 영속화됨 (재부팅·앱 재설치에도 유지) — 한 번 등록하면 끝
- 카메라 웹앱에서 클릭 4번으로도 등록 가능

## 4. 이벤트 → 데이터 시퀀스 (권장 사용 패턴)

```
차 진입·정지(3초)
  카메라 ──[monitordiff]──→  Occupied.2=True
  수신측 ──[GET XML]──────→  2번째 space: plate 빈값, ev=unknown  (판독 중)
번호·EV 확정 (수 초)
  카메라 ──[monitordiff]──→  Occupied.2 펄스 (False,True)
  수신측 ──[GET XML]──────→  plate=49허5678, ev=no, violation=true → 알람 ON
출차 (구역 90% 이탈 순간)
  카메라 ──[monitordiff]──→  Occupied.2=False
  수신측 ──[GET XML]──────→  2번째 space 초기화 → 알람 판단 재계산
```

## 5. 타이밍 요약

| 단계 | 지연 |
|---|---|
| 주차 확정 | 정지 후 3초 |
| 번호+EV 확정 | 정상 수 초 (교착 시 4초 주기 재도전) |
| 출차 확정 | 구역 90% 이탈 시 **즉시** (감지 드랍 시 폴백: 부재 ~4초) |
| 벨 전달 | 즉시 (monitordiff 푸시) |
