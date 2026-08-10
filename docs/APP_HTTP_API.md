# OpenSDK 앱에 HTTP API 만들기

`object_detect` 앱이 `/parking_tune` 같은 REST 엔드포인트를 여는 방법의 전체 해설.
WiseAI 의 `/configuration/*` 가 하는 것과 같은 구조를 우리 앱에 직접 만드는 법이다.

---

## 1. 전체 그림 — 요청이 앱까지 오는 길

```
클라이언트 (curl / 브라우저 / Pi)
  │  GET http://192.168.0.5/opensdk/object_detect/parking_tune
  │  (digest 인증: admin / •••)
  ▼
카메라 웹서버 (SUNAPI)
  │  /opensdk/<AppID>/<path> 패턴을 보고 그 앱으로 라우팅
  ▼
AppDispatcher (OpenSDK 플랫폼 컴포넌트)
  │  등록표(어느 path 를 어느 컴포넌트가 받는지)를 보고 FCGI 이벤트 발송
  ▼
우리 앱 SampleComponent::HandleHttpRequest(Event*)
  │  PATH_INFO 로 분기 → 응답 작성
  ▼
응답이 역순으로 클라이언트에게
```

핵심: **URL 의 `<AppID>` 뒤 경로가 통째로 우리 앱에 넘어온다.** 앱은
① 그 경로를 미리 **등록**하고 ② 요청이 오면 **처리**한다. 두 단계뿐이다.

---

## 2. 단계 ① — 경로 등록 (RegisterURI)

앱 초기화 때 한 번, 열고 싶은 경로마다 AppDispatcher 에 등록 이벤트를 보낸다.
[sample_component.cc](../app/src/sample_component/sample_component.cc) 의 `RegisterURI()`:

```cpp
void SampleComponent::RegisterURI() {
  // 허용 메서드 목록 — 이 목록에 없는 메서드는 AppDispatcher 가 거절한다
  Vector<String> methods;          // GET 전용
  methods.push_back("GET");
  Vector<String> gp;               // GET + POST (쓰기 있는 엔드포인트용)
  gp.push_back("GET");
  gp.push_back("POST");

  // 등록 한 건 = OpenAPIRegistrar 하나 (경로, 내 컴포넌트 이름, 메서드 목록)
  auto* ptune = new ("OpenAPI")
      IAppDispatcher::OpenAPIRegistrar(String("/parking_tune"), GetInstanceName(), gp);
  SendNoReplyEvent("AppDispatcher",
                   static_cast<int32_t>(IAppDispatcher::EEventType::eRegisterCommand),
                   0, ptune);
}
```

주의점:

- **`new ("OpenAPI")`** — 플랫폼 메모리 풀 태그. 일반 `new` 아님.
- 경로는 **한 세그먼트**로 (`/parking_tune`). 기존 엔드포인트 전부 이 관례
  (`/parking_roi`, `/rawmeta`, …). 하위 경로가 필요하면 쿼리스트링으로 (`?ch=0`).
- 쓰기 동작이 있으면 메서드 목록에 **POST 를 반드시 포함** — 빼먹으면 POST 가
  앱에 도달하기 전에 405 로 죽는데, 앱 로그엔 아무것도 안 남아서 헤맨다.

---

## 3. 단계 ② — 요청 처리 (HandleHttpRequest)

등록된 경로로 요청이 오면 앱에 이벤트가 오고, 우리는 한 함수에서 분기한다:

```cpp
bool SampleComponent::HandleHttpRequest(Event* event) {
  if (event->IsReply()) return true;
  auto* oas = reinterpret_cast<OpenAppSerializable*>(event->GetBaseObjectArgument());

  std::string path = oas->GetFCGXParam("PATH_INFO").c_str();   // "/parking_tune"

  if (path == "/parking_tune") {
    // ... 처리 ...
  }
  return true;
}
```

`oas` 하나로 요청의 모든 것을 읽고 응답의 모든 것을 쓴다:

### 요청에서 읽을 수 있는 것

| 호출 | 내용 | 예 |
|---|---|---|
| `oas->GetFCGXParam("PATH_INFO")` | 경로 | `/parking_tune` |
| `oas->GetFCGXParam("QUERY_STRING")` | `?` 뒤 전부 (직접 파싱) | `ch=0&clear=1` |
| `oas->GetMethod()` | `GET` / `POST` … | 분기용 |
| `oas->GetRequestBody()` | POST 본문 (raw 문자열) | JSON 등 |

쿼리 파싱은 라이브러리 없이 수동이다 (관례):

```cpp
std::string qs = oas->GetFCGXParam("QUERY_STRING").c_str();
int ch = 0;
size_t p = qs.find("ch=");
if (p != std::string::npos) ch = atoi(qs.c_str() + p + 3);
```

### 응답에 쓸 수 있는 것

| 호출 | 내용 |
|---|---|
| `oas->SetResponseBody(str, len)` | 본문 (기본 200) |
| `oas->SetStatusCode(400)` | 상태코드 (본문 설정 **전에**) |
| `oas->AddResponseHeader("Content-type", "application/json")` | 헤더 |
| `oas->SetResponseBody(data, OpenAppResponseType::FILE)` | 바이너리(JPEG 등) |

---

## 4. 실전 예제 — `/parking_tune` 전체 흐름

"주차 판정 파라미터를 런타임에 조정"이 요구사항. 완성된 모습:

```bash
# 현재값
curl -s --digest -u admin:'PW' "http://192.168.0.5/opensdk/object_detect/parking_tune"
# → {"overlap":0.65,"dwell_ms":2000,"grace_ms":2000,"exit_cover":0.10,
#    "presence_miss":3,"presence_period_ms":1500,"presence_conf":0.60,"burst_margin":0.35}

# 부분 갱신 — 보낸 키만 바뀜, 즉시 적용, 재시작 후에도 유지
curl -s --digest -u admin:'PW' -X POST -H "Content-Type: application/json" \
  -d '{"overlap":0.50,"presence_miss":0}' \
  "http://192.168.0.5/opensdk/object_detect/parking_tune"
```

구현 부품 4개:

### ④-1. 값의 그릇 — [park_tune.h](../app/src/sample_component/includes/core/park_tune.h)

```cpp
struct ParkTune {
  double overlap = cfg::kParkOverlapFrac;   // 초기값 = 컴파일 타임 상수
  ...
  std::string ToJson() const;                // GET 응답용
  bool FromJson(const std::string& body);    // POST 본문 → 부분 갱신 + 범위 검증
  void Load(const std::string& path);        // 부팅 시 파일에서 복원
  void Save() const;                         // 갱신 성공 시 파일로
};
```

범위 검증이 안전판: `overlap` 은 0.30~0.95 밖이면 그 키만 무시. 미친 값으로
판정이 죽는 사고를 API 층에서 차단한다.

### ④-2. 사용처 연결

판정 코드가 상수 대신 이 구조체를 읽게 바꾼다. ParkingZone 은 포인터 주입:

```cpp
parking_.SetTune(&park_tune_);                    // 초기화 때 1회
// parking_zone.h 내부: cfg::kParkDwellMs → TDwell() (tune_ 있으면 그 값)
```

### ④-3. 영속화

```cpp
park_tune_.Load("../storage/parking_tune.txt");   // 부팅 시 (파일 없으면 기본값)
park_tune_.Save();                                // POST 성공 시
```

`storage/` 는 앱 재설치에도 살아남는 앱 전용 공간이라 튜닝이 유지된다.

### ④-4. 핸들러

```cpp
} else if (path == "/parking_tune") {
  if (std::string(oas->GetMethod().c_str()) == "POST") {
    std::string rb(oas->GetRequestBody().c_str());
    if (park_tune_.FromJson(rb)) {
      park_tune_.Save();
      EmitEvent(0, std::string("[tune] applied: ") + park_tune_.ToJson());  // 디버거 로그
    } else {
      oas->SetStatusCode(400);
      oas->SetResponseBody("no valid field — GET /parking_tune for keys");
      return true;
    }
  }
  std::string b = park_tune_.ToJson();      // GET/POST 공통 — 최종값 반환
  oas->AddResponseHeader("Content-type", "application/json");
  oas->SetResponseBody(b.c_str(), b.size());
}
```

패턴 포인트: **POST 도 마지막에 현재값 전체를 돌려준다** — 클라이언트가 뭐가
실제로 반영됐는지 응답만 보고 확인 가능 (범위 밖 키는 옛값 그대로 보임).

---

## 5. 현재 앱의 엔드포인트 목록

베이스: `http://<카메라IP>/opensdk/object_detect` + digest 인증

| 경로 | 메서드 | 역할 |
|---|---|---|
| `/parking_status` | GET | 구역별 실시간 상태 XML (Pi 폴링) |
| `/parking_roi` | GET/POST | 구역 목록 / 추가(`?ch=`) / 삭제(`?delete=ID`) / 전체삭제(`?clear=1`) |
| `/parking_tune` | GET/POST | **주차 판정 런타임 튜닝** (이 문서의 예제) |
| `/config` | GET/POST | 사람/차량 감지 on/off |
| `/detections` | GET | 현재 감지 박스 JSON (`?ch=`) |
| `/snapshot` | GET | 라이브 스냅샷 JPEG (`?ch=`) |
| `/platelist` `/plate` | GET | 저장 크롭 갤러리 (개수 / `?n=` 이미지) |
| `/lastplate` | GET | 마지막 번호판 크롭 |
| `/platetext` | GET | 마지막 OCR 결과 JSON |
| `/isev` | GET | `?plate=` 등록·EV 여부 JSON |
| `/rawmeta` `/rawevents` | GET | 진단 — 메타데이터 원본 / 이벤트 누적 |
| `/eventlog` | GET | 디스크 이벤트 로그 |
| `/sysinfo` `/lsdownload` `/candlist` `/cand` | GET | 진단·검증용 |

---

## 6. 함정 모음 (실측으로 얻은 것)

1. **POST 등록 누락** → 405 인데 앱 로그 무흔적. 메서드 목록부터 의심.
2. **`SetStatusCode` 는 본문보다 먼저** — 뒤에 부르면 200 으로 나간 뒤라 무시됨.
3. **본문 파싱은 수동** — JSON 라이브러리 없음. 키 `find` + `atof` 관례
   (`park_tune.h` 의 `Num()` 참고). 키 이름이 다른 키의 부분문자열이 되지 않게 명명.
4. **PUT 은 안 쓴다** — 플랫폼 메서드 목록 관례가 GET/POST. 쓰기 = POST.
5. **응답으로 검증** — WiseAI 의 imageQuality 처럼 "Success 인데 무반영"이
   플랫폼엔 존재한다. 우리 API 는 POST 응답에 최종값을 실어 그 자리에서 확인.
6. **핸들러는 짧게** — HTTP 이벤트는 메타데이터 처리와 같은 스레드 계열로 온다.
   무거운 일(OCR 등)은 상태만 바꾸고 유지보수 루프가 하게 넘길 것.
7. **로그는 EmitEvent** — 설정 변경 같은 상태 변화는 디버거 뷰어에 흔적을 남겨야
   "언제 바뀌었지"를 추적할 수 있다 (`[tune] applied: {...}`).

---

## 7. 새 엔드포인트 추가 체크리스트

```
□ RegisterURI() 에 OpenAPIRegistrar 등록 (쓰기 있으면 gp=GET+POST)
□ HandleHttpRequest() 에 path 분기 추가
□ 쓰기면: 파싱 + 범위 검증 + 즉시 적용 + Save() + EmitEvent 로그
□ 응답: Content-type 헤더 + 최종 상태 반환
□ 빌드 → 설치 → curl GET/POST 로 검증 (POST 후 GET 재확인)
□ 이 문서 5절 표에 한 줄 추가
```
