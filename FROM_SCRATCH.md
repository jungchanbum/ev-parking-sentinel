# 처음부터 다시 만들기 — 깡통 프로젝트 → object_detect (완전판)

> `opensdk_new_project` 로 갓 만든 **빈 껍데기 앱**을 지금의 object_detect(움직임 감지 + 웹 오버레이 + 라이브 영상 + 차량 + DEBUG VIEWER)까지 만드는 전 과정.
> **바꿀 파일만** 다룬다. 안 건드리는 파일은 아예 안 나온다.
>
> 용어가 어려우면 → [BASICS.md](BASICS.md) / 데이터 흐름 → [DATAFLOW.md](DATAFLOW.md) / 구현 상세 → [README.md](README.md)

---

## 0. 시작점

```bash
opensdk_new_project -n object_detect -v 1.0 -s 26.05.19 -c cv5
```
| 옵션 | 뜻 |
|------|-----|
| `-n object_detect` | 앱(=폴더/컨테이너) 이름 |
| `-v 1.0` | 버전 |
| `-s 26.05.19` | SDK 버전 |
| `-c cv5` | 칩(SOC). 이 카메라는 `cv5` |

**깡통 앱의 기본 동작**: 웹에서 `/writeeventlog`, `/checksetting` 두 개만 받음. 카메라 AI와는 연결 0.

---

## 1. 바꿀 파일 — 딱 6개

| # | 파일 | 무엇을 | 난이도 |
|---|------|--------|--------|
| 1 | `config/app_manifest.json` | 앱 이름 확인만 | ⭐ |
| 2 | `app/src/PLifeCycleManagermanifest.json` | 카메라 부품 연결선 추가 | ⭐⭐ |
| 3 | `app/src/sample_component/manifests/SampleComponent_manifest_instance_0.json` | 구독/발신 배선 | ⭐⭐ |
| 4 | `app/src/sample_component/includes/sample_component.h` | 설계도(함수·변수 목록) | ⭐⭐ |
| 5 | `app/src/sample_component/sample_component.cc` | **로직 전부** | ⭐⭐⭐⭐ |
| 6 | `app/html/index.html` | 웹 UI | ⭐⭐⭐ |

> 🚫 **나머지 파일은 절대 안 건드린다.** 특히:
> - `app/src/sample_component/CMakeLists.txt` — **깡통 상태가 정답.** 여기에 HttpRequester를 넣으면 앱이 통째로 죽는다 (8장 사건 ①).
> - `app/CMakeLists.txt`, `toolchain.cmake`, `typedef_application.h`, `i_sample_component.h`, `AppDispatcher_*.json`, 인증서 등 — 자동/뼈대. 손대면 사고.

---

## 2. STEP 1 — `config/app_manifest.json`

`-n object_detect` 로 만들었으면 **이미 맞다.** 확인만.

```json
{ "AppName": "object_detect", "AppVersion": "1.0", "Permission": [] }
```

**왜?** `Permission: []` 비어도 된다 — 메타데이터 수신, 스냅샷 모두 별도 권한이 필요 없다.

---

## 3. STEP 2 — `PLifeCycleManagermanifest.json` (배선도 ①)

**역할**: 우리 앱이 카메라의 **어떤 부품과 연결**될지 선언.

#### 🟢 최종 상태 (이렇게 만든다)

```json
{
    "SkeletonPortNumber": "auto",
    "ContainerName": "object_detect",
    "AcceptLocalOnly": false,
    "SchedulerNames": ["EComponents::eScheduler1", "SampleComponentScheduler"],
    "RemoteContainerNames": [
        { "ContainerName": "System", "Address": "localhost", "PortNumber": 8587 }
    ],
    "RemoteComponentNames": [
        { "LocalComponentName": "Stub::Dispatcher::OpenAPI",
          "ContainerName": "System", "RemoteComponentName": "OpenAPIDispatcher" },
        { "LocalComponentName": "2009004",
          "ContainerName": "System", "RemoteComponentName": "2009004" },
        { "LocalComponentName": "OpenPlatform",
          "ContainerName": "System", "RemoteComponentName": "EComponents::eOpenPlatform" },
        { "LocalComponentName": "MetadataManager_{Ch}",
          "ContainerName": "System", "RemoteComponentName": "MetadataManager_{Ch}" }
    ],
    "Channel": "System"
}
```

#### ✅ 딱 3군데만 바뀜 — 정확히 어디냐면

**1) `"AcceptLocalOnly": false,` 한 줄 추가**
- 위치: `"ContainerName"` 바로 **아래**
- 왜: 외부 PC의 `remote_debug_viewer`가 접속하게 허용

**2) `MetadataManager_{Ch}` 블록 하나 추가** (핵심!)
- 위치: `RemoteComponentNames` 목록의 **맨 끝** (OpenPlatform 블록 뒤)
- ⚠️ 앞 블록(OpenPlatform) 끝에 **쉼표 `,` 붙이는 거 잊지 마** (JSON은 항목 사이에 쉼표 필수)
- 왜: **AI 결과 방송국에 선을 연결.** 이게 없으면 메타데이터를 못 받아 앱이 아무것도 못함

**3) `"Channel": "System"` 한 줄 추가** (핵심!)
- 위치: `RemoteComponentNames` 배열이 끝난 **뒤**, 맨 마지막 `}` **앞**
- ⚠️ 앞의 `]` 뒤에 **쉼표 `,` 붙여야 함**
- 왜: `{Ch}` 템플릿을 쓰려면 필수. **없으면 이벤트 큐가 안 만들어진다** 💀

> 💀 **함정 ①**: `MetadataManager_0,1,2,3`을 **직접 4줄로 쓰고** `"Channel": 0` 두면
> → 앱은 켜지는데 `EventQueue is not allocated`가 무한 반복되고 **메타데이터를 하나도 못 받는다.**
> 반드시 **`{Ch}` + `"Channel": "System"`** 조합. (공식 샘플 `metadata_sample` 방식)

---

## 4. STEP 3 — `SampleComponent_manifest_instance_0.json` (배선도 ②)

**역할**: 내가 **누구를 구독(Source)** 하고 **누구에게 보낼지(Receiver)**. (유튜브 구독목록)

#### 🟢 최종 상태 (이렇게 만든다)

```json
{
  "LibraryFileName": "libsample_component.so",
  "Instance": {
    "InstanceName": "SampleComponent",
    "SchedulerName": "SampleComponentScheduler",
    "ReceiverNames": [
      "AppDispatcher",
      "OpenPlatform"
    ],
    "SourceNames": [
      { "Source": "MetadataManager_{Ch}", "GroupName": "Metadata::OpenApp" },
      { "Source": "OpenPlatform", "GroupName": "AppMessage" },
      { "Source": "2009002", "GroupName": "AppInfo" }
    ],
    "Channel": "System",
    "ModelPath": "../res/models/",
    "SettingPath": "../storage/settings/"
  }
}
```

#### ✅ 바뀐 곳 정확히

**1) `ReceiverNames` (내가 보낼 대상)**
- `{ "SymbolName": "LogManager", ... }` 줄 **삭제** → 대신 `"OpenPlatform"` 넣음
- 왜: LogManager는 안 씀. `OpenPlatform`은 스냅샷 요청 보낼 대상

**2) `SourceNames` (내가 구독할 소식통) — 통째로 교체**
- 깡통의 `OpenPlatform`/`SettingChange` 한 줄 **삭제**
- 대신 **3줄 추가**:
  - `MetadataManager_{Ch}` / `Metadata::OpenApp` → **AI 좌표 받는 통로** (핵심!)
  - `OpenPlatform` / `AppMessage` → 스냅샷 완료 응답 받기
  - `2009002` / `AppInfo` → **앱 ID** 받기 (스냅샷 요청에 필요)

**3) `"Channel"` 값 변경**
- `0` → `"System"` 으로
- 왜: `{Ch}` 템플릿과 짝. 안 맞추면 큐 사망 💀 (STEP 2와 같은 이유)

> 💡 **Source vs Receiver 다시**
> - **Source = 구독** → 나에게 이벤트가 **들어옴**
> - **Receiver = 발신** → 내가 이벤트를 **내보냄**

> 💡 **Source vs Receiver**
> - **Source = 구독** → 나에게 이벤트가 **들어옴** (MetadataManager)
> - **Receiver = 발신** → 내가 이벤트를 **내보냄** (AppDispatcher, OpenPlatform)
>
> 💡 **`2009002`** = 우리 앱 자신의 LifeCycleManager 번호. `PLifeCycle`에 따로 안 적어도 내부에 있음.

> 🗑️ 깡통에 있던 `LogManager` 수신자, `OpenPlatform`/`SettingChange` 소스는 **빼도 됨** (우린 안 씀).

---

## 5. STEP 4 — `sample_component.h` (설계도)

**역할**: 이 컴포넌트에 **어떤 함수·변수**가 있는지 목록. (내용은 .cc에)

### 추가할 것

```cpp
#include <cstdint>      // ← 자료구조/정수타입 위해 추가
#include <deque>
#include <map>
#include <vector>

class SampleComponent : public Component, public ISampleComponent {
 private:
  // 함수 목록
  void HandleMetadataEvent(Event* event);               // 메타데이터 수신
  void ProcessObjects(int channel, const std::string& xml);  // ★ 파싱+판정
  void EmitEvent(int channel, const std::string& msg);  // 이벤트 출력
  void RegisterURI();                                   // HTTP 창구 열기
  bool HandleHttpRequest(Event* event);                 // HTTP 응답

  // 객체 1명의 이동 기록장
  struct ObjectTrack {
    std::deque<std::pair<float,float>> centers;  // 최근 6프레임 좌표
    uint64_t last_seen = 0;
    bool moving = false;
    uint64_t moving_until = 0;   // ★ 3초 유지 타이머 (이 시각까지 계속 moving)
  };
  // 웹에 넘길 박스 1개
  struct Detection { long id; bool moving; float l,t,r,b; };  // 좌표 0~1 정규화

  // 상태 변수 (모두 채널 4칸 배열)
  std::map<int, ObjectTrack> tracks_[4];  // [채널][ObjectId] → 기록장
  uint64_t tick_[4] = {0,0,0,0};
  double coord_scale_[4] = {1,1,1,1};     // 좌표 스케일 자동감지
  bool meta_diag_done_[4] = {false,false,false,false};
  std::vector<Detection> latest_[4];      // 채널별 최신 박스 (/detections용)
  double frame_w_[4] = {0,0,0,0};
  double frame_h_[4] = {0,0,0,0};
  std::string app_id_;                    // 스냅샷용 앱 ID
  std::string last_jpeg_[4];              // 스냅샷 캐시 (라이브 영상 더블버퍼)
  uint64_t last_snap_trigger_[4] = {0,0,0,0};
};
```

### 자료구조 왜 이렇게?

| 문법 | 왜 |
|------|-----|
| `struct { ... }` | 여러 값을 묶은 꾸러미 (좌표+상태) |
| `std::deque` | 앞뒤로 넣고 빼는 줄 → 최근 6프레임 유지에 딱 |
| `std::map<키,값>` | 사전. `ObjectId`로 그 사람 기록장 찾기 |
| `std::vector` | 늘었다 줄었다 하는 목록 |
| `이름_[4]` | 채널 0~3 각각 (다방향 카메라라 채널이 여러 개) |

---

## 6. STEP 5 — `sample_component.cc` (로직) ★ 핵심

### 6.1 include

```cpp
// 깡통에 있던 것 중 유지
#include "dispatcher_serialize.h"       // JSON, HTTP 응답 도구
#include "i_app_dispatcher.h"           // HTTP 이벤트
#include "i_log_manager.h"              // 원격 디버그 메시지

// 추가
#include "i_p_metadata_manager.h"       // ★ 메타데이터
#include "i_p_open_platform_manager.h"  // 스냅샷
#include "life_cycle_manager_openapp.h" // 앱 ID
#include <chrono>                       // 3초 타이머
#include <cmath>                        // sqrt (거리)
#include <fstream>                      // 파일 (스냅샷)
#include <sstream>
```

### 6.2 `Initialize()` — 앱 켤 때 1번

```cpp
bool SampleComponent::Initialize() {
  setvbuf(stdout, NULL, _IONBF, 0);   // ★ 안 하면 printf 로그 안 보임! (8장 함정)
  RegisterURI();                       // /detections, /snapshot 창구 열기
  printf("[object_detect] app started\n");
  return Component::Initialize();      // 부모 것도 실행 (필수)
}
```

### 6.3 `ProcessAEvent()` — 쪽지 분배

```cpp
switch (event->GetType()) {
  case eMetadataRequest:  HandleMetadataEvent(event); break;  // ✅ 메타데이터
  case eHttpRequest:      HandleHttpRequest(event);   break;  // 웹 요청
  case eInformAppInfo:    /* app_id_ 저장 */           break;  // ✅ 앱 ID
  default: Component::ProcessAEvent(event); break;
}
```
> 깡통의 `eNetworkSettingChanged` 는 지워도 됨.

### 6.4 메타데이터 꺼내기

```cpp
void SampleComponent::HandleMetadataEvent(Event* event) {
  auto attachment = event->GetAttachment<IPMetadataManager::MetadataOutput>();
  int ch = attachment->channel();                   // 채널 번호
  std::string xml(attachment->output().c_str());    // ONVIF XML
  ProcessObjects(ch, xml);
}
```
> **핵심 개념**: 오는 건 **ONVIF XML** (JSON 아님!). WiseAI가 "지금 여기 뭐 있다"를 초당 5번 던짐.
> 자세히 → [DATAFLOW.md](DATAFLOW.md)

### 6.5 `ProcessObjects()` — 이 앱의 심장

```
1) <tt:Scale> 로 프레임 크기 계산  (frame_w = 2 ÷ scale)
2) <tt:Object ObjectId="N"> 반복
     - >Human< 또는 >Vehicle< 인지 확인 → 아니면 skip (얼굴/머리 거름)  ← 차량도 추적
     - <tt:CenterOfGravity> → 중심좌표, <tt:BoundingBox> → 박스(0~1 정규화)
3) ObjectId 별 기록장에 좌표 push (6개 넘으면 앞에서 버림)
4) 6개 모이면 → 5프레임 전과 거리 비교
     거리 > 프레임폭×3%  → moving=true, 3초 타이머 갱신, 이벤트 발사(전환 순간 1번)
     3초 지나고 조용     → moving=false                                  ← 3초 유지
5) 15프레임 이상 안 보인 객체 삭제
6) **움직이는 것만** latest_[ch] 에 저장 (정지 객체는 박스 안 뜸 = B 방식)
```

**움직임 판정 핵심 (왜 이렇게?)**
- **5프레임 전과 비교**: 옆 프레임끼리는 AI 떨림(노이즈)에 오판. 간격 벌려야 진짜 이동만 남음
- **좌표스케일 자동감지**: 해상도 무관하게 임계값을 비율로 계산
- **3초 유지 타이머**: 한 번 움직이면 3초간 계속 moving. 걷다 잠깐 멈칫해도 안 꺼짐
- **움직이는 것만 표시(B)**: 정지 객체는 박스도 이벤트도 없음. "진짜 움직이는 것만"이 목적.
  `objectdetected` 같은 "객체 등장" 이벤트는 없앰 (스팸 방지 + 경량화)

### 6.6 이벤트 출력 (2경로)

```cpp
void SampleComponent::EmitEvent(int ch, const std::string& msg) {
  printf("[EVENT ch%d] %s\n", ch, msg.c_str());              // ① 디버그 로그 CGI
  SendTargetEvents(ILogManager::remote_debug_message_group,  // ② DEBUG VIEWER
      (int32_t)ILogManager::EEvent::eRemoteDebugMessage, 0,
      new ("") Platform_Std_Refine::SerializableString(msg.c_str()));
}
```

### 6.7 HTTP 창구 등록

```cpp
void SampleComponent::RegisterURI() {
  Vector<String> methods; methods.push_back("GET");
  // /detections 와 /snapshot 두 개 등록 (AppDispatcher 에게 "이 주소 나한테 줘")
}
```
> 깡통의 `/writeeventlog`, `/checksetting` 은 지우고 우리 것으로 교체.

### 6.8 `/detections` — 감지 박스 JSON

```cpp
// latest_[ch] 를 JSON으로 만들어 응답
// {"ch":0,"objects":[{"id":2018,"moving":true,"box":[0.59,0.0,0.74,0.07]}]}
```

### 6.9 `/snapshot` — 라이브 영상 (서버측 더블버퍼)

**왜 필요?** 브라우저가 카메라 CGI를 직접 못 부름(보안 차단, 8장 사건 ④).
→ 앱이 **카메라 안에서** 스냅샷을 떠서 서빙.

```cpp
// ① 다음 프레임을 비동기로 미리 주문 (SendNoReplyEvent → 스레드 안 막힘)
// ② 파일이 완성된 JPEG(FFD8...FFD9)이면 메모리 캐시(last_jpeg_) 갱신
// ③ 캐시된 완성 프레임을 즉시 응답 (대기 없음 → 박스도 안 멈춤)
```
> 💡 `SendReplyEventWait`(기다림) 대신 `SendNoReplyEvent`(안 기다림)를 쓰는 게 핵심.
> 기다리면 그동안 메타데이터 스레드가 멈춰서 **박스까지 버벅**인다.
>
> ⚠️ **한계**: 스냅샷 API는 해상도 옵션이 없어 항상 풀해상도(~285KB). 영상이 완전 부드럽진 않다.
> 진짜 부드럽게 하려면 raw 영상을 받아 작게 인코딩해야 함 (미래 작업, README 10장).

> **전체 코드**: `app/src/sample_component/sample_component.cc`

---

## 7. STEP 6 — `app/html/index.html` (웹 UI)

깡통엔 버튼 2개짜리 허접 페이지. **통째로 교체.**

### 절대 유지할 부분 ⚠️

```js
const paths = window.location.pathname.split('/');
const app_id = paths[5];                              // ← 이 구조 유지!
const base_uri = `.../opensdk/${app_id}`;
```
앱 API 주소를 만드는 공식. **깡통 그대로 두고** 그 아래만 우리 걸로.
> 💀 이것 때문에 반드시 **[Go App]** 으로 열어야 함. 로컬파일/`/wmf/`로 열면 `app_id`를 못 찾아 전부 실패.

### 구조

```html
<div class="stage">          <!-- position:relative, aspect-ratio:16/9 -->
  <img id="cam">             <!-- 라이브 스냅샷 (아래층) -->
  <canvas id="ov">           <!-- 감지 박스 (위층, 투명) -->
</div>
<script>
  const active = () => !document.hidden;   // ★ 탭이 보이는 중인지 (경량화)
  // 루프 A (150ms): /detections 폴링 → 캔버스에 네모 (부드러움)
  // 루프 B (더블버퍼): /snapshot 폴링 → 다음 프레임 미리 받고 준비되면 즉시 교체
  // 두 루프 모두 active() 아니면 요청 스킵 → 탭 숨기면 카메라 부하 0
</script>
```

**왜 두 루프를 분리?** 영상은 무거워 느리고, 박스는 가벼워 빠르다.
분리하면 **영상이 느려도 박스는 부드럽게** 따라간다.

**경량화 — 탭 숨기면 정지 (`document.hidden`)**: 페이지를 열어둔 채 다른 탭을 보거나
최소화하면 **스냅샷·감지 요청이 멈춘다.** 지금 이 앱 경량화의 90%가 이것 —
"안 볼 때는 카메라를 괴롭히지 않는다."

> **전체 코드**: `app/html/index.html`

---

## 7.5 STEP 6.5 — DEBUG VIEWER 연동 (CLI 쪽, 선택)

앱 이벤트(`움직이는 사람 감지!`)를 **PC에서 `remote_debug_viewer` CLI**로 보고 싶을 때만.
디버그 로그 CGI로도 볼 수 있으니 **필수는 아님.**

두 부분이 필요하다:

### (A) 앱 쪽 — 이미 STEP 5에서 했음
- `EmitEvent()` 가 `SendTargetEvents(remote_debug_message_group, ...)` 로 쏨 (6.6)
- `PLifeCycle` 에 `"AcceptLocalOnly": false` (STEP 2) — 외부 뷰어 접속 허용
- ✅ 이 둘만 있으면 앱은 준비 끝

### (B) 뷰어 CLI 쪽 — 여기 2개 파일 수정
위치: `sdk/CLI/remote_debug_viewer/app/`

**① `bin/PLifeCycleManagermanifest.json`** — 뷰어가 접속할 대상 지정

```json
{
    "LocalAddress": "192.168.0.11",          ← 뷰어를 돌리는 내 PC의 IP (★ 본인 IP로!)
    "SkeletonPortNumber": 42771,
    "ContainerName": "remote_debug_viewer",
    "AcceptLocalOnly": false,
    "SchedulerNames": ["DebugViewerScheduler"],
    "RemoteContainerNames": [
        { "ContainerName": "System", "Address": "192.168.0.5", "PortNumber": 8587 },
        { "ContainerName": "object_detect", "Address": "192.168.0.5", "PortNumber": 0 }
    ],
    "RemoteComponentNames": [
        { "LocalComponentName": "Viewer_1",
          "ContainerName": "object_detect", "RemoteComponentName": "SampleComponent" }
    ]
}
```
바꾼 것:
- `RemoteContainerNames` 에 **`object_detect` 컨테이너 추가** (Address = 카메라 IP)
- `RemoteComponentNames` 에 **`Viewer_1` → `object_detect::SampleComponent` 매핑 추가**
- `LocalAddress` 를 **내 PC IP** 로 (카메라 IP 아님! 뷰어가 도는 컴퓨터)

**② `res/models/DebugViewer_manifest_instance_0.json`** — 뷰어가 구독할 소스

```json
{
  "LibraryFileName" : "libdebug_viewer.so",
  "Instance": {
      "InstanceName" : "DebugViewer",
      "SchedulerName" : "DebugViewerScheduler",
      "ReceiverNames" : [],
      "SourceNames" : [
        { "Source": "Viewer_1", "GroupName": "RemoteDebugMessage" }
      ],
      "Channel": 0,
      "ModelPath": "../res/models",
      "SettingPath": "../res/models"
  }
}
```
바꾼 것:
- `SourceNames` 에 **`Viewer_1` / `RemoteDebugMessage` 구독 추가**
  (`Viewer_1` 은 ①에서 object_detect::SampleComponent 로 매핑됨 → 그 앱의 디버그 메시지를 받음)

### 왜 이렇게?

```
우리앱 SampleComponent
   └ SendTargetEvents(RemoteDebugMessage, "움직이는 사람 감지!")
        └→ 뷰어가 Viewer_1(=object_detect::SampleComponent)의
             RemoteDebugMessage 그룹을 원격 구독 → 화면에 뜸
```

### 뷰어 실행 (PC에서)
```bash
cd sdk/CLI/remote_debug_viewer/app/bin
sh run_life_cycle_manager.sh
```

> 💡 뷰어는 **재빌드 필요 없음** (JSON만 고치면 됨). 앱만 재빌드하면 됨.

---

## 8. 빌드 & 설치

```bash
cd object_detect
docker compose up          # 컴파일 + 패키징 → object_detect.cap
# 환경변수 안 먹으면:
# SDK_VER=26.05.19 APP_NAME=object_detect SOC=cv5 docker compose up
```
1. 카메라 웹 → **설정 → 오픈플랫폼 → 애플리케이션** → `.cap` 업로드 → **Start**
2. **[Go App]** 클릭 ← 반드시!

로그 확인:
```
http://<카메라IP>/stw-cgi/debugcgi?msubmenu=data&action=view&command_arg=opensdk_object_detect
```

---

## 9. 완료 체크리스트

빌드 전:
- [ ] `PLifeCycle` 에 `MetadataManager_{Ch}` + `"Channel": "System"` 있나?
- [ ] 인스턴스도 `"Channel": "System"` 인가? (`0` 아님!)
- [ ] 인스턴스 소스에 `MetadataManager_{Ch}` / `Metadata::OpenApp` 있나?
- [ ] 인스턴스에 `OpenPlatform` 수신자 + `AppMessage`/`AppInfo` 소스 있나? (스냅샷용)
- [ ] `sample_component/CMakeLists.txt` 를 **안 건드렸나?**
- [ ] `Initialize()` 에 `setvbuf(stdout, NULL, _IONBF, 0)` 있나?

설치 후:
- [ ] `[object_detect] app started` 보이나?
- [ ] `EventQueue is not allocated` **안** 보이나?
- [ ] 사람 지나갈 때 `움직이는 사람 감지!` 뜨나?

---

## 10. 자주 하는 실수 (전부 실제로 겪음)

| 실수 | 증상 | 해결 |
|------|------|------|
| `MetadataManager_0,1,2,3` 하드코딩 | `EventQueue is not allocated` 무한반복 | `{Ch}` + `"Channel":"System"` |
| `"Channel": 0` 그대로 | 위와 동일 | `"System"` 으로 |
| `CMakeLists.txt` 에 HttpRequester 추가 | 초기화 예외 → 앱 사망 | **깡통 상태 유지** |
| `setvbuf` 누락 | 로그가 안 보임 (앱은 잘 돎) | `Initialize()` 첫 줄 |
| `/wmf/`·로컬파일로 앱페이지 열기 | 버튼/API 전부 404 | **[Go App]** |
| 메타데이터를 JSON으로 파싱 | 조용히 아무것도 안 됨 | **ONVIF XML** 이다 |
| 브라우저→카메라 CGI 직접 호출 | 200인데 이미지 아님(58B 거부) | 앱이 대신 받아 전달(프록시) |
| 스냅샷을 `SendReplyEventWait`로 매 요청 | 박스까지 버벅 | `SendNoReplyEvent` + 캐시 |
| WiseAI 체크박스 끄면 감지 멈출 거라 착각 | 계속 잡힘 | 체크박스=알람규칙. 메타데이터는 계속 나옴 |
| `OpenAPIDispatcher not found` 보고 당황 | — | **무해함.** 정상 동작 |

---

## 11. 전체 요약 (1장)

```
opensdk_new_project -n object_detect -v 1.0 -s 26.05.19 -c cv5
        ↓
① PLifeCycleManagermanifest.json
      + MetadataManager_{Ch}  + Channel:System  + AcceptLocalOnly:false
② SampleComponent_manifest_instance_0.json
      + MetadataManager_{Ch} 소스   Channel:0→System
      + OpenPlatform 수신/소스  + 2009002 AppInfo   (스냅샷/앱ID용)
③ sample_component.h    추적/감지/스냅샷 자료구조
④ sample_component.cc   메타데이터 수신 → XML파싱 → 움직임판정(3초유지)
                        → 사람+차량 → 이벤트(2경로) → /detections + /snapshot
⑤ index.html           영상(더블버퍼) + 캔버스 박스 오버레이
        ↓
docker compose up  →  .cap  →  업로드  →  Start  →  [Go App]
```

**핵심 통찰**: 코드보다 **매니페스트(①②)가 훨씬 중요**했다.
코드는 멀쩡한데 배선이 틀려서 며칠 날렸다.
새 프로젝트는 **①② 배선부터 맞추고, 로그에 메타데이터가 들어오는지 확인한 뒤** 로직을 짜라.
