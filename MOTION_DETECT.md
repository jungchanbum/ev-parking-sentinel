# object_detect — WiseAI 움직이는 객체 감지 앱

한화비전(Hanwha Vision) 카메라 **PNM-C16083RVQ** 에서 동작하는 Wisenet Open Platform(OpenSDK) 애플리케이션.
WiseAI가 감지한 객체(사람·차량) 중 **실제로 움직이는 것만** 골라내어 이벤트를 발생시키고,
웹 페이지에서 **라이브 영상 위에 그 움직이는 객체 박스만 오버레이**해서 보여준다.
(정지한 객체는 이벤트도, 박스도 없음)

- 대상 카메라: PNM-C16083RVQ (멀티디렉셔널, SUNAPI 2.6.6, ONVIF 24.06)
- SDK: Wisenet Open Platform 25.02.25 / SOC `cv5`
- 언어: C++ (앱), HTML/JS (웹 UI)

### 📚 문서 3종

| 문서 | 내용 | 이럴 때 |
|------|------|---------|
| **[BASICS.md](BASICS.md)** | 용어·개념·원리를 비유로 설명 | 용어가 낯설 때 **← 여기부터** |
| **[DATAFLOW.md](DATAFLOW.md)** | 실제 로그 숫자로 데이터 흐름 추적 | **"이게 어떻게 도는지" 감이 안 올 때** |
| **MOTION_DETECT.md** (이 파일) | 무엇을 어떻게 만들었나 + 트러블슈팅 | 구현 내용이 궁금할 때 |
| **[FROM_SCRATCH.md](FROM_SCRATCH.md)** | 깡통 프로젝트 → 이 앱까지 단계별 | 직접 다시 만들어볼 때 |

---

## 1. 무엇을 만들었나

| 기능 | 설명 | 상태 |
|------|------|------|
| 객체 감지 수신 | WiseAI 메타데이터에서 **사람(Human)·차량(Vehicle)** 추출 | ✅ 검증됨 |
| **움직임 판정** | 5프레임 전 좌표와 비교해 움직이는 것만 선별 | ✅ 검증됨 |
| **3초 유지** | 한 번 움직이면 3초간 계속 moving (잠깐 멈칫해도 유지) | ✅ 검증됨 |
| `움직이는 사람/차량 감지!` 이벤트 | **움직이는 것에만** 발생 (정지 객체는 무시) | ✅ 검증됨 |
| **움직이는 것만 표시** | 정지 객체는 박스도 이벤트도 없음 (B 방식) | ✅ 검증됨 |
| 디버그 로그 CGI 출력 | 브라우저 주소창으로 로그 확인 | ✅ 검증됨 |
| `/detections` HTTP API | 현재 **움직이는** 박스를 JSON 제공 | ✅ 검증됨 |
| 웹 오버레이 UI | 채널 탭 + 캔버스 박스 오버레이 | ✅ 검증됨 |
| `/snapshot` 라이브 영상 | 앱이 카메라 내부에서 스냅샷 떠서 서빙(더블버퍼) | ✅ 검증됨 |
| 경량화 | 탭 숨기면 요청 정지(카메라 부하 0) + 죽은 코드 제거 | ✅ 검증됨 |
| DEBUG VIEWER 연동 | `remote_debug_viewer` CLI로 이벤트 보기 | ⚠️ 구현 완료, 미검증 |

> ⚠️ DEBUG VIEWER는 코드/설정은 다 됐지만 카메라+PC 연결 확인 전 단계.
>
> ❌ **미완**: 영상이 풀해상도 JPEG(~285KB)이라 완전 매끄럽진 않음.
> 저해상도 스트림(`SPMgrSnapshot` 프로파일)은 미래 작업 (10장).

---

## 2. 전체 아키텍처

```
┌───────────────────────── 카메라 (192.168.0.5) ─────────────────────────┐
│                                                                        │
│   WiseAI 엔진                                                          │
│      │  ONVIF 메타데이터 (XML, ~5fps, 채널별)                          │
│      ▼                                                                 │
│   MetadataManager_0..3                                                 │
│      │  eMetadataRequest 이벤트                                        │
│      ▼                                                                 │
│  ┌──────────── object_detect 앱 (우리가 만든 것) ────────────┐         │
│  │  SampleComponent                                          │         │
│  │   ├─ HandleMetadataEvent()  ← 메타데이터 수신             │         │
│  │   ├─ ProcessObjects()       ← XML 파싱 + 움직임 추적      │         │
│  │   ├─ EmitEvent()            ← 이벤트 출력(2경로)          │         │
│  │   └─ HandleHttpRequest()    ← /detections, /snapshot      │         │
│  └───────────────────────────────────────────────────────────┘         │
│      │              │                    │                             │
│      │ printf       │ SendTargetEvents   │ HTTP 응답                   │
│      ▼              ▼                    ▼                             │
│  디버그로그CGI   RemoteDebugMessage   AppDispatcher                    │
└────────┼──────────────┼────────────────────┼───────────────────────────┘
         │              │                    │
         ▼              ▼                    ▼
   브라우저 주소창  remote_debug_viewer   앱 웹페이지(Go App)
                     (PC에서 실행)        영상+박스 오버레이
```

### 왜 이런 구조인가
- **메타데이터 경로만 좌표를 준다.** HTTP 폴링(`eventstatus.cgi`)이나 MotionDetection 이벤트는
  `State: true/false` 불리언만 줘서 "어디에 있는지"를 알 수 없다 → 움직임 판정 불가.
- 그래서 `MetadataManager` 의 `Metadata::OpenApp` 그룹을 구독해서 **객체별 좌표**를 받는다.

---

## 3. 한화 OpenSDK 기본 개념 (필수 배경지식)

OpenSDK 앱은 **컴포넌트 + 이벤트 + 매니페스트** 구조다.

| 개념 | 설명 |
|------|------|
| **Component** | 기능 단위 (우리 앱은 `SampleComponent` 하나) |
| **Event** | 컴포넌트 간 메시지. `ProcessAEvent()`에서 타입별 분기 |
| **Scheduler** | 컴포넌트가 돌아가는 스레드. 이벤트 큐를 가짐 |
| **Manifest(JSON)** | 코드가 아니라 **JSON으로 배선(wiring)** 을 정의 ← 핵심! |

### 매니페스트 3종
| 파일 | 역할 |
|------|------|
| `app/src/PLifeCycleManagermanifest.json` | 컨테이너 이름, 스케줄러 목록, **원격 컴포넌트 매핑** |
| `app/src/sample_component/manifests/SampleComponent_manifest_instance_0.json` | 내 컴포넌트의 **수신자(Receiver)/소스(Source)** 배선 |
| `config/app_manifest.json` | 앱 이름/버전/권한 |

**Receiver** = 내가 이벤트를 보낼 대상. **Source** = 내가 구독할 이벤트 발생지.

---

## 4. 데이터 흐름 (한 프레임 기준)

> 📗 **이 흐름을 실제 로그 숫자로 하나씩 따라가는 상세판 → [DATAFLOW.md](DATAFLOW.md)**

```
1. WiseAI가 프레임 분석 → ONVIF XML 생성
2. MetadataManager_N 이 eMetadataRequest 이벤트 발생
3. SampleComponent::ProcessAEvent() 가 받아서 HandleMetadataEvent() 호출
4. attachment->output() 으로 XML 문자열 획득, attachment->channel() 로 채널 번호
5. ProcessObjects(ch, xml) 실행:
   a. <tt:Scale> 파싱 → 프레임 픽셀 크기 계산
   b. <tt:Object ObjectId="N"> 순회 → Human 만 필터
   c. <tt:CenterOfGravity> → 중심좌표, <tt:BoundingBox> → 박스
   d. ObjectId 별로 최근 6프레임 좌표 저장
   e. 5프레임 전과 거리 비교 → 임계값 초과 시 "움직임"
   f. 상태 전이 시 EmitEvent() 발사
   g. 오버레이용 정규화 박스를 latest_[ch] 에 저장
6. 브라우저가 /detections?ch=N 폴링 → latest_[ch] 를 JSON으로 응답
```

---

## 5. 핵심 구현

### 5.1 메타데이터 수신

```cpp
void SampleComponent::HandleMetadataEvent(Event* event) {
  if (event == nullptr || event->IsReply()) return;
  auto attachment = event->GetAttachment<IPMetadataManager::MetadataOutput>();
  if (!attachment) return;
  int ch = attachment->channel();                    // 채널 번호
  std::string xml(attachment->output().c_str());     // ONVIF XML 문자열
  ProcessObjects(ch, xml);
}
```

### 5.2 실제 메타데이터 포맷 (ONVIF XML) ⭐

**가장 중요한 발견.** 문서에는 C++ 구조체(`object_id`, `rect{sx,sy,ex,ey}`)만 나와 있어서
JSON일 거라 착각했는데, **실제로 오는 건 ONVIF 표준 XML** 이었다.

```xml
<tt:MetadataStream>
 <tt:VideoAnalytics>
  <tt:Frame UtcTime="2026-07-16T07:32:48.344Z">
   <tt:Transformation>
     <tt:Translate x="-1.0" y="1.0"/>
     <tt:Scale x="0.000772" y="-0.001316"/>   <!-- 좌표계 변환 -->
   </tt:Transformation>
   <tt:Object ObjectId="22020">               <!-- ← 추적 ID -->
    <tt:Appearance>
     <tt:Shape>
      <tt:BoundingBox left="1343" top="0" right="1783" bottom="128"/>
      <tt:CenterOfGravity x="1563" y="64"/>   <!-- ← 중심좌표(픽셀) -->
     </tt:Shape>
     <tt:Class>
      <tt:Type Likelihood="0.53">Human</tt:Type>  <!-- Human/Face/Head/Other -->
     </tt:Class>
    </tt:Appearance>
   </tt:Object>
   ...
  </tt:Frame>
 </tt:VideoAnalytics>
</tt:MetadataStream>
```

**포인트**
- `ObjectId` = 프레임 간 동일 객체를 잇는 **추적 ID** (WiseAI가 알아서 유지해줌)
- `CenterOfGravity` = 중심좌표를 **이미 계산해서 줌** (직접 계산 불필요)
- 좌표는 **픽셀** 단위
- 프레임 크기 = `2 / |Scale|` (ONVIF 정규화 범위가 [-1,1]이라서)
  → `2 / 0.000772 ≈ 2591px`, `2 / 0.001316 ≈ 1520px`
- 한 사람이 `Human`(몸통) + `Head` + `Face` 로 **여러 객체**가 잡힘
  → `Human`만 필터해야 사람 1명 = 박스 1개

### 5.3 XML 파싱 (라이브러리 없이 문자열 스캔)

libxml2 링크 없이 가볍게 처리. 포맷이 규칙적이라 문자열 검색으로 충분.

```cpp
size_t o = xml.find("<tt:Object ObjectId=\"", pos);
long id = atol(xml.c_str() + id_start);          // 추적 ID

size_t next_obj = xml.find(kObjTag, id_start);   // 이 객체의 끝 경계
size_t obj_end  = (next_obj == npos) ? xml.size() : next_obj;

size_t h = xml.find(">Human<", id_start);        // Human 만 통과
if (h == npos || h >= obj_end) continue;

size_t c = xml.find("<tt:CenterOfGravity x=\"", id_start);
double cx = atof(xml.c_str() + c + kCog.size());
```

**주의점**
- 항상 `obj_end`(다음 `<tt:Object>` 위치)로 범위를 제한해야 **다음 객체의 값을 잘못 읽지 않음**
- `ImageRef`가 붙은 객체는 메인 박스가 `0,0,0,0` → `cx==0 && cy==0` 이면 skip
- `pos = id_start` 로 매 루프 전진 보장 (무한루프 방지)

### 5.4 움직임 감지 알고리즘 ⭐

```
객체마다 최근 6프레임 중심좌표를 deque에 보관
   ↓
6프레임 모이면: 현재좌표 ↔ 5프레임 전 좌표 거리 계산
   ↓
거리 > (좌표스케일 × 3%)  →  움직임 ON  → 이벤트 발사 + 3초 타이머 갱신
움직임 없이 3초 경과      →  움직임 OFF
   ↓
움직이는 것만 latest_[ch] 에 담음 (정지 객체는 박스 안 뜸 = B 방식)
   ↓
15프레임 이상 안 보이면 추적에서 제거 (메모리 관리)
```

```cpp
if ((int)tr.centers.size() >= kCompareBack + 1) {
  double dist = sqrt(dx*dx + dy*dy);
  double move_th = coord_scale_[ch] * kMoveRatio;   // 2591 * 0.03 ≈ 78px
  uint64_t now_ms = NowMs();
  if (dist > move_th) {
    tr.moving_until = now_ms + kMovingHoldMs;        // 움직임 → 3초 연장
    if (!tr.moving) {                                // 전환 순간에만 이벤트 1번
      tr.moving = true;
      EmitEvent(ch, "움직이는 사람/차량 감지! (chN idM)");
    }
  } else if (tr.moving && now_ms >= tr.moving_until) {
    tr.moving = false;                               // 3초간 조용 → 해제
  }
}
...
if (tr.moving) dets.push_back({...});   // ★ 움직이는 것만 화면에 담음
```

**설계 의도**
- **왜 5프레임 전과 비교?** 인접 프레임끼리 비교하면 미세 떨림(노이즈)에 반응.
  ~1초(5프레임@5fps) 간격으로 봐야 "실제 이동"만 잡힘.
- **왜 3초 유지(`kMovingHoldMs`)?** 걷다가 잠깐 멈칫하거나 AI 좌표가 튈 때
  moving이 껌뻑거리는 걸 방지. 한 번 움직이면 3초간은 계속 moving으로 본다.
  (거리 임계값 하나로 ON/OFF 하던 히스테리시스를 **시간 기반으로 대체** — 더 직관적)
- **왜 좌표스케일 자동 감지?** 해상도가 달라져도 코드 수정 없이 동작.
- **왜 움직이는 것만 표시(B)?** "진짜 움직이는 객체만" 이 앱의 목적.
  정지 객체 박스를 안 그려서 화면도 깔끔하고 전송량도 준다.

**검증된 실제 동작**
- `ObjectId 22020` (걸어가는 사람): x좌표 `1563 → 1738 → 1899 …` → **🔴 표시됨** ✅
- `ObjectId 1981` (서 있는 사람): `1003 → 992 → 1027` (미세 변동) → **무시됨(박스 없음)** ✅

### 5.5 이벤트 출력 — 2가지 경로

```cpp
void SampleComponent::EmitEvent(int channel, const std::string& msg) {
  printf("[EVENT ch%d] %s\n", channel, msg.c_str());          // ① 디버그 로그 CGI
  SendTargetEvents(ILogManager::remote_debug_message_group,   // ② DEBUG VIEWER
                   (int32_t)ILogManager::EEvent::eRemoteDebugMessage, 0,
                   new ("") Platform_Std_Refine::SerializableString(msg.c_str()));
}
```

| 경로 | 보는 방법 |
|------|-----------|
| ① `printf` | `http://192.168.0.5/stw-cgi/debugcgi?msubmenu=data&action=view&command_arg=opensdk_object_detect` |
| ② `SendTargetEvents` | PC에서 `remote_debug_viewer` CLI 실행 |

> `Initialize()`에서 `setvbuf(stdout, NULL, _IONBF, 0)` 필수 —
> 안 하면 printf가 버퍼에 갇혀 로그가 안 보인다.

### 5.6 HTTP API

```cpp
void SampleComponent::RegisterURI() {              // Initialize()에서 호출
  Vector<String> methods; methods.push_back("GET");
  auto* uri = new ("OpenAPI")
      IAppDispatcher::OpenAPIRegistrar(String("/detections"), GetInstanceName(), methods);
  SendNoReplyEvent("AppDispatcher",
      (int32_t)IAppDispatcher::EEventType::eRegisterCommand, 0, uri);
}
```

**`GET /detections?ch=N`** → 현재 감지 박스 (정규화 0~1)
```json
{"ch":0,"objects":[{"id":2018,"moving":true,"box":[0.52,0.00,0.68,0.08]}]}
```

**`GET /snapshot?ch=N`** → 라이브 JPEG (아래 5.7 참고)

호출 주소: `http://<카메라IP>/opensdk/<app_id>/detections?ch=0`

### 5.7 라이브 영상 — 서버측 스냅샷 프록시 ⭐

**브라우저에서 카메라 CGI를 직접 부르면 막힌다** (8장 참고).
그래서 **앱이 카메라 내부에서 스냅샷을 떠서** 자기 엔드포인트로 서빙한다.

```cpp
// JSON: {jpeg_path, channel, app_name}
auto res = SendReplyEventWait(                       // ← 동기 호출 (완료까지 대기)
    (uint64_t)Component::EReceivers::eOpenPlatformManager,
    (int32_t)IPOpenPlatformManager::EAppEventType::eAppSnapshotJpeg, 0,
    new ("Query") Platform_Std_Refine::SerializableString(sb.GetString()));

if (res) {                                           // 파일로 저장 완료
  std::ifstream ifs(jpath);
  std::ostringstream oss; oss << ifs.rdbuf();
  oas->SetStatusCode(200);
  oas->SetResponseBody(oss.str(), OpenAppResponseType::FILE);  // content-type 자동
}
```

- `app_name` 에는 **앱 ID**가 필요 → `eInformAppInfo` 이벤트로 받아서 `app_id_` 에 보관
- 이를 위해 매니페스트에 `OpenPlatform` 수신자 + `AppInfo` 소스 추가 필요

### 5.8 웹 오버레이 UI

```
<div class="stage">              position:relative, aspect-ratio:16/9
  ├─ <img id="cam">              라이브 스냅샷 (absolute, inset:0)
  └─ <canvas id="ov">            감지 박스 (absolute, inset:0, 위에 겹침)
```

- **감지 폴링 150ms** (박스가 부드럽게 움직임) / **스냅샷 500ms** (~2fps, 카메라 부하 완화)
  → 두 루프를 분리해서 영상이 느려도 박스는 부드럽다
- 박스 좌표는 앱이 **0~1로 정규화**해서 주므로, JS는 캔버스 크기만 곱하면 됨
  → 해상도/화면크기 무관하게 정확히 겹침
- 🟢 초록 = 감지된 사람 / 🔴 빨강+글로우 = 움직이는 사람

---

## 6. 파일별 변경 요약

### 앱 (`object_detect/`)
| 파일 | 내용 |
|------|------|
| `app/src/sample_component/sample_component.cc` | **핵심 로직 전부** — XML 파싱, 움직임 추적, 이벤트, HTTP API, 스냅샷 |
| `app/src/sample_component/includes/sample_component.h` | 추적 상태(`ObjectTrack`), 감지결과(`Detection`), 멤버 변수 |
| `app/src/PLifeCycleManagermanifest.json` | `MetadataManager_{Ch}`, `Channel:System`, `AcceptLocalOnly:false` |
| `app/src/sample_component/manifests/SampleComponent_manifest_instance_0.json` | 수신자/소스 배선 |
| `app/src/sample_component/CMakeLists.txt` | HttpRequester 번들링 제거 |
| `app/html/index.html` | 오버레이 웹 UI 전체 |

### 디버그 뷰어 CLI (`CLI/remote_debug_viewer/`)
| 파일 | 내용 |
|------|------|
| `app/bin/PLifeCycleManagermanifest.json` | `object_detect` 컨테이너 + `Viewer_1` 매핑 |
| `app/res/models/DebugViewer_manifest_instance_0.json` | `Viewer_1` 의 `RemoteDebugMessage` 구독 |

### 삭제한 것
- `app/libs/http_requester/`, `HttpRequester_*.json` — 초기화 예외 유발 (8장 참고)

---

## 7. 빌드 / 배포 / 확인

```bash
# 1) 빌드 + 패키징 (한화 OpenSDK 도커)
docker compose up        # → object_detect.cap 생성

# 2) 카메라에 설치
#    카메라 웹 → 설정 → 오픈플랫폼 → 애플리케이션
#    → .cap 업로드 → Start

# 3) 앱 웹페이지 열기
#    같은 화면의 [Go App] 버튼   ← 반드시 이걸로! (URL에 app_id가 들어감)

# 4) 로그 확인
#    http://192.168.0.5/stw-cgi/debugcgi?msubmenu=data&action=view&command_arg=opensdk_object_detect
```

**정상 동작 로그**
```
[object_detect] app started, waiting for metadata...
[EVENT ch0] [진단] ch0 Human 4, frame=2591x1520
[EVENT ch0] 움직이는 사람 감지! (ch0 id2018)     ← 🎯 (움직이는 것만 발사)
```

---

## 8. 트러블슈팅 기록 (제일 값진 부분) ⭐

실제로 부딪힌 문제와 원인/해결. **이 프로젝트의 진짜 학습 포인트.**

### ① `EventQueue is not allocated` 무한 반복 — 앱이 메타데이터를 못 받음

**증상**
```
Event Send Failed PSkeleton:165, Exception : SampleComponent 25035001 Event
  <- EventQueue is not allocated     (초당 수십 번 반복)
```

**해석**
- `25035001` 이벤트가 초당 수십 번 온다 = **메타데이터는 잘 오고 있다**
- 근데 SampleComponent의 **이벤트 큐가 할당이 안 됨** → 다 버려짐

**원인 (2개)**
1. **채널 설정 오류** — 잘 도는 `metadata_sample`은 `"Channel":"System"` +
   `MetadataManager_{Ch}` (템플릿) 인데, 우리는 `MetadataManager_0/1/2/3` 하드코딩 +
   `"Channel": 0` → 프레임워크가 채널별 큐를 제대로 할당 못 함
2. **불필요한 HttpRequester** — 초기화 중 예외를 유발

**해결**
```json
// PLifeCycleManagermanifest.json
{ "LocalComponentName": "MetadataManager_{Ch}", ... }
"Channel": "System"
```
+ HttpRequester 관련 파일/CMake 설정 전부 제거

**교훈**: 잘 도는 공식 샘플(`metadata_sample`)과 **1:1로 diff** 하는 게 가장 빠른 길이었다.

---

### ② 메타데이터가 JSON이 아니라 ONVIF XML

**증상**: 파서가 아무것도 못 잡음 (조용히 실패)

**원인**: SDK 문서에는 C++ 구조체(`object_id`, `rect{sx,sy,ex,ey}`)만 나와 있어서
JSON이라 착각. 실제 `output()` 은 **ONVIF `<tt:MetadataStream>` XML**.

**해결**: 실제 로그(`[META raw]`)를 찍어서 포맷 확인 후 XML 파서로 전면 재작성.

**교훈**: **문서보다 실제 데이터가 진실.** 추측하지 말고 raw 로그부터 찍자.

---

### ③ `OpenAPIDispatcher name component is not found` 예외

**증상**: 부팅 로그에 예외가 뜸

**결론**: **무해함.** `metadata_sample` 등 공식 샘플도 동일한 배선이라 같은 예외가 뜬다.
실제로 `/detections` HTTP 요청은 정상 동작했다.

**교훈**: 로그에 예외가 뜬다고 다 원인이 아니다. **증상과 인과를 분리**해야 한다.

---

### ④ 브라우저에서 카메라 스냅샷 CGI 직접 호출이 막힘 ⭐

**증상**
- 주소창에 `http://192.168.0.5/stw-cgi/video.cgi?msubmenu=snapshot&action=view&Channel=0`
  → **사진 정상 표시** ✅
- 같은 URL을 앱 페이지의 `<img>` / `fetch` 로 요청 → **실패** ❌
- 진단 결과: `fetch 200 · text/plain · 58B` (JPEG 대신 58바이트 거부 텍스트)

**시도했으나 실패한 것들**
| 시도 | 결과 |
|------|------|
| `<img src>` | onerror |
| `fetch` + blob | 200이지만 non-image |
| `Accept: image/*` 헤더 추가 | 동일 |
| `referrerpolicy="no-referrer"` (Referer 제거) | 동일 |

**원인**: 카메라가 **요청 컨텍스트**(`Sec-Fetch-*` 계열 헤더 등)를 검사해서
페이지 내부 요청을 거부. 이 헤더들은 **브라우저가 강제로 붙여서 JS로 제거 불가**.
→ 브라우저에서 카메라 CGI 직접 호출은 **근본적으로 불가능**.

**해결 (아키텍처 전환)**
```
❌ 브라우저 ──직접──> 카메라 CGI          (보안 차단)
✅ 브라우저 ──> 우리 앱 /snapshot ──> 카메라 내부 SDK 스냅샷
```
앱은 **카메라 안에서** 도니까 브라우저 보안 제약이 없다.
`/detections` 가 잘 되는 걸로 앱 엔드포인트 경로는 이미 검증됨.

**교훈**: 클라이언트에서 막히면 **서버측(앱)에서 대신 가져와 프록시**한다. 웹 개발의 정석 패턴.

---

### ⑤ 앱 웹페이지는 반드시 [Go App]으로 열어야 함

**증상**: 버튼이 동작 안 함, `file://:/opensdk/...` 같은 이상한 URL

**원인**: `index.html` 이 URL 경로에서 `app_id` 를 추출하는 구조
```js
const app_id = paths[5];   // /home/setup/opensdk/html/object_detect/index.html
const base_uri = `.../opensdk/${app_id}`;
```
- 로컬 `file://` 로 열면 → `app_id = undefined`
- 카메라 기본 웹뷰어(`/wmf/`)에서 열어도 → 경로가 달라서 실패

**해결**: 반드시 **오픈플랫폼 → 앱 → [Go App]** 으로 열기.

---

## 9. 튜닝 파라미터

`app/src/sample_component/sample_component.cc` 상단:

| 상수 | 기본값 | 의미 | 조절 방향 |
|------|--------|------|-----------|
| `kHistoryFrames` | 6 | 객체별 보관 프레임 수 | `kCompareBack`보다 커야 함 |
| `kCompareBack` | 5 | 몇 프레임 전과 비교 | ↑ 둔감·안정 / ↓ 민감·노이즈 |
| `kMoveRatio` | 0.03 | 움직임 임계 (화면의 3%) | ↑ 둔감 / ↓ 민감 |
| `kMovingHoldMs` | 3000 | 움직임 후 유지 시간(ms) | ↑ 오래 빨강 유지 |
| `kStaleFrames` | 15 | 미검출 시 추적 삭제 | ↑ 오래 기억 |

웹 UI (`app/html/index.html`): 감지 폴링 `150ms`, 스냅샷 `MIN_GAP 90ms`(≈10fps), 탭 숨기면 정지

---

## 10. 앞으로 할 수 있는 것

- **영상 매끄럽게 (제일 큰 숙제)**: 지금은 풀해상도 JPEG(~285KB)이라 무거움.
  `SPMgrSnapshot` 스냅샷 매니저에 **`Profile`(해상도) 지정**이 있어 저해상도 프로파일로
  작은 JPEG을 직접 받으면 부드러워짐. 단, 이걸 쓰는 샘플이 없어 **배선·응답형식·프로파일을
  역설계**해야 하는 큰 작업. (raw 영상은 하드웨어 DMA 버퍼라, OpenCV는 cv5에 없음, libjpeg은
  헤더가 없어 각각 막힘 — 조사 완료)
- **차량 세분화**: 지금은 `Vehicle` 통합. `승용차/버스/트럭` 등 세부 클래스 분리 가능
- **이벤트 승격**: 로그가 아니라 SUNAPI 알람 / OSD 표시 / 외부 HTTP 전송
- **방향/속도**: 좌표 이력이 있으니 이동 벡터·속도 계산 가능 (침입/배회 판정 등)

---

## 부록: 유용한 주소

```
디버그 로그   http://192.168.0.5/stw-cgi/debugcgi?msubmenu=data&action=view&command_arg=opensdk_object_detect
콘솔모드 끄기 http://192.168.0.5/stw-cgi/debugcgi?msubmenu=console&action=off   (후 재부팅)
감지 API     http://192.168.0.5/opensdk/<app_id>/detections?ch=0
스냅샷 API   http://192.168.0.5/opensdk/<app_id>/snapshot?ch=0
카메라 스냅샷 http://192.168.0.5/stw-cgi/video.cgi?msubmenu=snapshot&action=view&Channel=0
             (주소창에서만 동작. 페이지 내부 호출은 차단됨 — 8장 ④ 참고)
```

참고 문서: `sdk/Document/HTML/HanwhaVision_OpenPlatform_Programming_Guide_*.html`
(3.7 Debugging Log CGI / 4.3 Start an Application),
`sdk/Document/PDF/HanwhaVision_OpenPlatform_SDK_API_*.pdf` (3.2.2.2 Frame Metadata / 3.3.3 Snapshot JPEG)

참고 샘플: `sdk/SampleApplication/metadata_sample` (메타데이터 수신 기준),
`sdk/SampleApplication/snapshot_jpeg` (스냅샷 API 패턴)
