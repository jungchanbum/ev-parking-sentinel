# 데이터가 흐르는 길 — 진짜 숫자로 따라가기

> "WiseAI가 물체를 인식하면 그때의 상태를 ONVIF XML로 가지고 있는 거야?"
> → **아니다.** 이 오해부터 풀고 시작한다.
>
> 이 문서는 **실제 카메라 로그에서 뽑은 진짜 숫자**로 전 과정을 따라간다.

---

## 0. 제일 중요한 오해 풀기 ⭐

### ❌ 틀린 그림

```
   WiseAI
   ┌─────────────────┐
   │ 22020번 사람:    │   ← WiseAI가 이런 걸
   │  위치 (1563,64)  │      저장해놓고 있다?
   │  상태: 움직임    │
   └─────────────────┘
```

### ✅ 맞는 그림

```
WiseAI = 저장소가 아니라 "중계 아나운서"

프레임1 → "지금 22020번이 (1563,64)에 있습니다" → 던지고 잊음
프레임2 → "지금 22020번이 (1589,62)에 있습니다" → 던지고 잊음
프레임3 → "지금 22020번이 (1605,61)에 있습니다" → 던지고 잊음
         ↑ 초당 5번, 매번 새 보고서. 과거는 기억 안 함
```

**WiseAI가 주는 건 "그 순간의 사진 설명서" 한 장이다.**
- "움직인다/멈췄다" 같은 건 **안 알려준다**
- 과거 위치도 **안 알려준다**
- 그냥 **"지금 이 순간 어디에 뭐가 있다"** 만 반복해서 말한다

### 그럼 "움직임"은 누가 판단하나?

> ## 👉 **우리 앱이 한다. 그게 이 프로젝트의 전부다.**

```
WiseAI (남이 만듦)          우리 앱 (우리가 만듦)
──────────────────         ─────────────────────
"지금 여기 있다" ────────►  받아서 노트에 적어둠
"지금 여기 있다" ────────►  또 적어둠
"지금 여기 있다" ────────►  또 적어둠          ← 6장 모임
                            ↓
                            첫 장과 마지막 장 비교
                            "어 많이 움직였네!" → 이벤트 발사
```

**WiseAI는 기억이 없고, 우리 앱이 기억을 만든다.**

### 딱 하나, WiseAI가 기억하는 것

**`ObjectId` (번호표)** 는 유지해준다.
```
프레임1: 저 사람 = 22020번
프레임2: (같은 사람이네) = 22020번   ← 같은 번호 유지!
프레임3: 22020번
```
이게 없으면 우린 "이 사람이 아까 그 사람인지" 알 수가 없다.
→ **WiseAI = 위치 알려주기 + 번호표 붙이기.**
→ **우리 앱 = 번호별로 기록하고 비교하기.**

---

## 1. 전체 와꾸 (한 장)

```
┌───────────────────── 카메라 안 ─────────────────────┐
│                                                      │
│  [WiseAI]                                            │
│    │ 0.2초마다 ONVIF XML 한 장 생성                  │
│    ▼                                                 │
│  [MetadataManager_1]  ← 방송국 (채널1 담당)          │
│    │ "eMetadataRequest" 쪽지 발송                    │
│    ▼                                                 │
│  ┌────── 우리 앱 (object_detect) ──────┐            │
│  │  받은편지함(큐)                       │            │
│  │      ▼                                │            │
│  │  ProcessAEvent()   쪽지 종류 확인     │            │
│  │      ▼                                │            │
│  │  HandleMetadataEvent()  껍질 까기     │            │
│  │      ▼                                │            │
│  │  ProcessObjects()  ★ 파싱+판정        │            │
│  │      ├─ 기록장에 좌표 적기            │            │
│  │      ├─ 6장 모이면 비교 → 이벤트      │            │
│  │      └─ latest_[1] 에 박스 저장       │            │
│  └───────────────────────────────────────┘            │
│                    ▲                                  │
│                    │ /detections?ch=1                 │
└────────────────────┼──────────────────────────────────┘
                     │
                [브라우저] 0.15초마다 물어봄 → 네모 그림
```

---

### 1-1. MetadataManager 는 우리 앱 **밖**에 있다 ⭐

```
┌─────────── 카메라 (한 대) ───────────────────────────┐
│                                                      │
│  ┌── System (카메라 본체 = 한화가 만든 것) ──┐       │
│  │   MetadataManager_0   ← ch0 담당 방송국   │       │
│  │   MetadataManager_1   ← ch1 담당 방송국   │       │
│  │   MetadataManager_2   ← ch2 담당 방송국   │       │
│  │   MetadataManager_3   ← ch3 담당 방송국   │       │
│  │   OpenAPIDispatcher, OpenPlatform ...     │       │
│  └───────────────┬───────────────────────────┘       │
│                  │ ← ★ 이 선을 매니페스트로 그어야 함 │
│  ┌───────────────▼───────────────────────────┐       │
│  │  object_detect (우리가 만든 앱)           │       │
│  │     SampleComponent                       │       │
│  └───────────────────────────────────────────┘       │
└──────────────────────────────────────────────────────┘
```

**같은 카메라 안이지만 서로 다른 "회사(컨테이너)"다.**
그래서 그냥은 못 듣고, **매니페스트에 연결선을 그어야** 방송이 들어온다:

```json
// PLifeCycleManagermanifest.json — "저 회사가 어디 있는지"
"RemoteContainerNames": [
  { "ContainerName": "System", "Address": "localhost", "PortNumber": 8587 }
]                                            ↑ 같은 기계 안(localhost)의 8587번 창구
```
```json
// SampleComponent_manifest_instance_0.json — "그 회사 방송 구독"
"SourceNames": [
  { "Source": "MetadataManager_{Ch}", "GroupName": "Metadata::OpenApp" }
]
```

> 💡 **`MetadataManager` 는 4개다** — 채널(렌즈)마다 하나씩.
> `{Ch}` 템플릿이 알아서 `_0`, `_1`, `_2`, `_3` 넷 다 구독해준다.
> 그래서 우리 코드의 `tracks_[4]`, `latest_[4]` 도 전부 **4칸 배열**이다.
>
> (실제 로그엔 `ch0`, `ch1` 만 데이터가 왔다. 나머지 렌즈는 조용했거나 감지 대상이 없었던 것.)

### 1-2. 객체가 없으면 메타데이터를 안 보내나? → **아니다, 계속 보낸다**

**님 로그의 실제 증거** — 아무도 없을 때도 프레임이 온다:

```xml
[META raw] ch=1 output=<?xml version="1.0" encoding="UTF-8"?><tt:MetadataStream ...>
  <tt:VideoAnalytics>
    <tt:Frame UtcTime="2026-07-16T07:33:08.666Z">
      <tt:Transformation>
        <tt:Translate x="-1.0" y="1.0"/>
        <tt:Scale x="0.000000" y="0.000000"/>     ← scale 이 0!
      </tt:Transformation>
    </tt:Frame>                                    ← <tt:Object> 가 하나도 없음!
  </tt:VideoAnalytics>
</tt:MetadataStream>
```

**빈 프레임**도 꼬박꼬박 온다. 우리 코드는 이걸 자연스럽게 무시한다:
```cpp
while (true) {
  size_t o = xml.find(kObjTag, pos);   // "<tt:Object ObjectId=" 찾기
  if (o == std::string::npos) break;   // 없으면 그냥 끝 → 아무 일도 안 일어남
  ...
}
```

> ⚠️ **빈 프레임의 `Scale` 이 `0` 인 게 함정.**
> `frame_w = 2 ÷ 0` = **무한대/에러**가 될 뻔했다. 그래서 코드에 방어벽이 있다:
> ```cpp
> if (std::fabs(sx) > 1e-9) frame_w_[ch] = 2.0 / std::fabs(sx);
> //  ↑ scale이 0에 가까우면 계산 안 하고 이전 값 유지
> ```

### 1-3. 메타데이터엔 "프레임" 말고 "이벤트"도 섞여 온다

같은 통로로 **다른 종류**도 온다 (실제 로그):

```xml
<tt:Event><wsnt:NotificationMessage>
  <wsnt:Topic>tns1:OpenApp/WiseAI/ObjectDetection</wsnt:Topic>   ← WiseAI 감지 on/off
  <tt:Data><tt:SimpleItem Name="State" Value="true"/>
           <tt:SimpleItem Name="ClassTypes" Value="Person"/></tt:Data>
</wsnt:NotificationMessage></tt:Event>
```
```xml
  <wsnt:Topic>tns1:VideoSource/MotionAlarm</wsnt:Topic>          ← 모션 감지
  <wsnt:Topic>tns1:AppMgmt/State</wsnt:Topic>                    ← 앱 상태 알림
```

| 종류 | 내용 | 우리 앱은? |
|------|------|-----------|
| `<tt:VideoAnalytics><tt:Frame>` | **객체 좌표** | ✅ 이것만 씀 |
| `<tt:Event>` ObjectDetection | 사람 있다/없다 (on/off) | ❌ 무시 (좌표가 없음) |
| `<tt:Event>` MotionAlarm | 움직임 있다/없다 | ❌ 무시 |
| `<tt:Event>` AppMgmt | 앱 시작/종료 알림 | ❌ 무시 |

**`<tt:Object ObjectId=` 가 없는 XML은 자동으로 걸러진다.** 따로 처리할 게 없다.

> 💡 **이래서 좌표가 필요했다.**
> `ObjectDetection` 이벤트는 "사람 있음/없음"만 알려준다.
> **어디에 있는지를 모르니 움직임 판정이 불가능**하다.
> 그래서 우린 `<tt:Frame>` 의 좌표를 쓴 것.

---

## 2. 실제 데이터로 한 프레임 따라가기

> 아래 숫자는 **전부 님 카메라 실제 로그**에서 가져온 것. (2026-07-16 07:32:52, 채널1)

### STEP 1 — WiseAI가 XML 한 장을 뱉는다

시각 `07:32:52.011`, 채널1. WiseAI가 방금 이 프레임을 분석하고 이렇게 보고한다:

```xml
<tt:MetadataStream>
 <tt:VideoAnalytics>
  <tt:Frame UtcTime="2026-07-16T07:32:52.011Z">

   <tt:Transformation>
     <tt:Translate x="-1.0" y="1.0"/>
     <tt:Scale x="0.000772" y="-0.001316"/>      ← ① 좌표계 정보
   </tt:Transformation>

   <tt:Object ObjectId="22020">                   ← ② 번호표
    <tt:Appearance>
     <tt:Shape>
      <tt:BoundingBox left="1539.0" top="1.0" right="1937.0" bottom="110.0"/>   ← ③ 네모
      <tt:CenterOfGravity x="1738.0" y="55.5"/>                                  ← ④ 중심점
     </tt:Shape>
     <tt:Class>
      <tt:Type Likelihood="0.61">Human</tt:Type>  ← ⑤ 사람!
     </tt:Class>
    </tt:Appearance>
   </tt:Object>

   <tt:Object ObjectId="22018" Parent="22020">    ← 같은 사람의 "머리"
     ... <tt:Type Likelihood="0.73">Head</tt:Type>    ← 사람 아님 → 우린 버림
   </tt:Object>

   <tt:Object ObjectId="22026" Parent="22020">    ← 같은 사람의 "얼굴"
     ... <tt:Type Likelihood="0.45">Face</tt:Type>    ← 버림
   </tt:Object>

  </tt:Frame>
 </tt:VideoAnalytics>
</tt:MetadataStream>
```

> 💡 **사람 1명인데 객체가 3개!**
> `Human`(몸통 22020) + `Head`(머리 22018) + `Face`(얼굴 22026)
> `Parent="22020"` = "나는 22020번의 일부야" 라는 뜻.
> → **`Human` 만 쓰면 사람 1명 = 박스 1개** 로 깔끔해진다.

### STEP 2 — 방송국이 쪽지를 보낸다

`MetadataManager_1` 이 이 XML을 들고 **쪽지(이벤트)** 를 만들어 우리 앱 받은편지함에 넣는다.

```
쪽지 종류: eMetadataRequest
쪽지 내용: { channel: 1, output: "<?xml version..." }
```

### STEP 3 — 우리 앱이 쪽지를 꺼낸다

```cpp
bool SampleComponent::ProcessAEvent(Event* event) {
  switch (event->GetType()) {
    case eMetadataRequest:              // ← "메타데이터 왔다" 쪽지구나!
      HandleMetadataEvent(event);       //    담당 함수 호출
      break;
```

### STEP 4 — 껍질 까서 알맹이 꺼내기

```cpp
void SampleComponent::HandleMetadataEvent(Event* event) {
  auto attachment = event->GetAttachment<IPMetadataManager::MetadataOutput>();
  int ch = attachment->channel();                     // → 1
  std::string xml(attachment->output().c_str());      // → "<?xml version..."
  ProcessObjects(ch, xml);                            // 진짜 일 시작
}
```

| 변수 | 실제 값 |
|------|---------|
| `ch` | `1` |
| `xml` | `<?xml version="1.0"...` (약 3000자) |

#### 🔬 이 3줄 완전 해부 (택배 비유)

```
event      = 도착한 택배 상자 📦
attachment = 상자 안의 서류 📄  ← "채널" 칸과 "본문" 칸이 있음
```

---

**1번째 줄**
```cpp
auto attachment = event->GetAttachment<IPMetadataManager::MetadataOutput>();
```

| 조각 | 뜻 |
|------|-----|
| `event->` | **"택배 상자의~"** |
| `GetAttachment` | **"첨부물 꺼내기"** |
| `<IPMetadataManager::MetadataOutput>` | **"이건 '메타데이터 서류' 양식이야"** 라고 알려주는 것 |
| `()` | 실행! |
| `auto` | **"결과 타입은 컴파일러가 알아서 써줘"** (이름이 너무 길어서) |
| `attachment` | 꺼낸 서류를 이 이름으로 부르겠다 |

> **`<...>` 이 왜 필요한가?**
> 상자 안 내용물은 이벤트 종류마다 다르다 (메타데이터 서류일 수도, HTTP 요청서일 수도).
> 그래서 **"이 상자에서 이 양식으로 꺼내줘"** 하고 **양식을 지정**해야 한다.
> 이걸 **템플릿(Template)** 이라고 부른다.

> **`auto` 를 안 쓰면?**
> ```cpp
> IPMetadataManager::MetadataOutput* attachment = event->GetAttachment<...>();
> //  ↑ 이렇게 길게 써야 함. 똑같은 걸 두 번 쓰니까 귀찮 → auto
> ```

> **`::` 는?** "소속". `IPMetadataManager::MetadataOutput`
> = "IPMetadataManager 네 집의 MetadataOutput" (BASICS 4장 기호표 참고)

---

**2번째 줄**
```cpp
int ch = attachment->channel();
```

| 조각 | 뜻 |
|------|-----|
| `attachment->` | **"그 서류의~"** |
| `channel()` | **"채널 칸 읽어줘"** → `1` 반환 |
| `int ch =` | 그 답을 `ch` 라는 **정수 상자**에 담기 |

결과: `ch` = `1`

---

**3번째 줄**
```cpp
std::string xml(attachment->output().c_str());
```

이건 3단계가 한 줄에 겹쳐있다. **안쪽부터** 읽어야 한다:

```cpp
attachment->output()      // ① "서류의 본문 칸 읽어줘" → XML 글자들 (SDK 전용 문자열)
                .c_str()  // ② "그걸 순수 글자배열로 바꿔줘"
std::string xml( ... );    // ③ 그 글자들을 표준 문자열 상자에 복사, 이름은 xml
```

| 조각 | 뜻 |
|------|-----|
| `output()` | 서류 본문(XML) 꺼내기 |
| `.c_str()` | **SDK 문자열 → 순수 글자배열**로 변환 |
| `std::string xml(...)` | 표준 문자열 `xml` 을 만들면서 그 내용으로 **채우기** |

> **왜 `.c_str()` 이 필요한가?**
> SDK는 자기만의 문자열 타입을 쓴다. 우리는 표준 `std::string` 으로 다루고 싶다.
> `.c_str()` 은 **둘 사이의 번역기**. (모든 문자열 타입이 공통으로 가진 기능)

> **`std::string xml(값)` 이 낯설다면**
> ```cpp
> std::string xml = "안녕";     // 이거랑
> std::string xml("안녕");      // 이거랑 똑같다 (C++ 두 가지 표기법)
> ```

---

**전체를 한국어로**
```cpp
void SampleComponent::HandleMetadataEvent(Event* event) {
  // 택배 상자에서 '메타데이터 서류' 양식으로 내용물을 꺼낸다
  auto attachment = event->GetAttachment<IPMetadataManager::MetadataOutput>();

  int ch = attachment->channel();                  // 서류의 '채널' 칸 → 1
  std::string xml(attachment->output().c_str());   // 서류의 '본문' 칸 → XML 글자들

  ProcessObjects(ch, xml);                         // "1번 채널의 이 XML 처리해!" 하고 넘김
}
```

### STEP 5 — `ProcessObjects()` 안에서 벌어지는 일

#### 5-a. 프레임 크기 계산

XML에서 `<tt:Scale x="0.000772" y="-0.001316"/>` 를 찾아서:

```
frame_w = 2 ÷ 0.000772 = 2591 픽셀
frame_h = 2 ÷ 0.001316 = 1520 픽셀
```

> **왜 2로 나누나?** ONVIF는 화면을 `-1 ~ +1` 로 표현하기로 약속했다.
> 폭이 `-1`에서 `+1`까지니까 전체 길이가 **2**. 그걸 scale로 나누면 픽셀 수가 나온다.

#### 5-b. 객체 순회 + Human 필터

```
발견: ObjectId=22020, Type=Human  → ✅ 통과
발견: ObjectId=22018, Type=Head   → ❌ 버림
발견: ObjectId=22026, Type=Face   → ❌ 버림
```

코드로는:
```cpp
size_t h = xml.find(">Human<", id_start);     // 이 객체 범위 안에 "Human"이 있나?
if (h == npos || h >= obj_end) continue;      // 없으면 다음 객체로
```

#### 5-c. 좌표 뽑기

```
ObjectId 22020 에서:
  CenterOfGravity x="1738.0" y="55.5"       → cx=1738.0, cy=55.5
  BoundingBox left=1539 top=1 right=1937 bottom=110
```

#### 5-d. 기록장에 적기

```cpp
tracks_[1][22020].centers.push_back({1738.0, 55.5});
```

이러면 22020번의 기록장이 이렇게 쌓인다 (**실제 로그 값**):

| # | 시각 | cx | cy |
|---|------|-----|-----|
| 1 | 07:32:51.144 | 1589.0 | 62.5 |
| 2 | 07:32:51.344 | 1605.5 | 61.0 |
| 3 | 07:32:51.611 | 1645.0 | 61.5 |
| 4 | 07:32:51.744 | 1672.5 | 57.0 |
| 5 | 07:32:52.011 | **1738.0** | **55.5** | ← 방금 추가
| 6 | ... | | |

> 6개를 넘으면 **제일 오래된 걸 앞에서 버린다** (`pop_front`).
> 항상 "최근 6장"만 유지 → 메모리가 안 늘어남.

#### 5-e. 6장 모이면 → 비교!

6번째가 들어온 순간 (`07:32:52.211`, cx=1790.5, cy=52.0):

```
          [1]              →              [6]
    (1589.0, 62.5)                  (1790.5, 52.0)
     5프레임 전                         지금

   dx = 1790.5 - 1589.0 = 201.5
   dy = 52.0 - 62.5 = -10.5

   거리 = √(201.5² + 10.5²) = √(40602 + 110) = 201.8 픽셀
```

임계값 계산:
```
임계값 = frame_w × 3% = 2591 × 0.03 = 77.7 픽셀
```

판정:
```
201.8 > 77.7  →  🎯 움직인다!
```

#### 5-f. 상태가 바뀌는 순간에만 이벤트 발사 (+ 3초 유지)

```cpp
if (dist > move_th) {
  tr.moving_until = now_ms + 3000;    // 움직임 → 3초 타이머 갱신
  if (!tr.moving) {                    // 안 움직였다가 이번에 움직였다!
    tr.moving = true;
    EmitEvent(ch, "움직이는 사람 감지! (ch1 id22020)");   // 🔔 딱 1번만!
  }
} else if (tr.moving && now_ms >= tr.moving_until) {
  tr.moving = false;                   // 3초간 조용 → 해제
}
```

> 💡 **왜 `!tr.moving` 을 확인하나?**
> 안 그러면 걷는 내내 **매 프레임마다** 이벤트가 터진다 (초당 5번!).
> "안 움직임 → 움직임" 으로 **바뀌는 순간에만** 알린다.
>
> 💡 **왜 3초 유지?** 걷다 잠깐 멈칫하거나 AI 좌표가 튀면 moving이 껌뻑거린다.
> 한 번 움직이면 3초간은 계속 moving으로 봐서 안정적으로 만든다.

로그에 찍히는 것:
```
[EVENT ch1] 움직이는 사람 감지! (ch1 id22020)
```

#### 5-g. 웹 오버레이용 박스 저장 — **움직이는 것만** (정규화)

```cpp
if (tr.moving) {        // ★ 움직이는 객체만 화면에 담는다 (정지 객체는 skip)
  dets.push_back({...});
}
```

```
BoundingBox: left=1539, top=1, right=1937, bottom=110
프레임 크기: 2591 × 1520

정규화 (픽셀 ÷ 프레임크기):
  l = 1539 ÷ 2591 = 0.5940
  t =    1 ÷ 1520 = 0.0007
  r = 1937 ÷ 2591 = 0.7476
  b =  110 ÷ 1520 = 0.0724
```

```cpp
latest_[1] = [ {id:22020, moving:true, l:0.5940, t:0.0007, r:0.7476, b:0.0724} ];
// ↑ 움직이는 22020번만 들어감. 서 있는 1981번 등은 여기 없음 → 박스 안 뜸
```

> **왜 0~1로 바꾸나?** 브라우저 화면 크기를 우리가 모르기 때문.
> "화면 왼쪽에서 59.4% 지점" 이라고 하면 화면이 크든 작든 항상 맞다.

### STEP 6 — 브라우저가 가져간다

브라우저가 0.15초마다:
```
GET http://192.168.0.5/opensdk/object_detect/detections?ch=1
```

앱이 `latest_[1]` 을 JSON으로 만들어 응답:
```json
{"ch":1,"objects":[{"id":22020,"moving":true,"box":[0.5940,0.0007,0.7476,0.0724]}]}
```

브라우저가 캔버스에 그림 (캔버스가 800×450 이라면):
```js
x = 0.5940 × 800 = 475px      // 왼쪽에서 475픽셀
y = 0.0007 × 450 = 0.3px      // 거의 맨 위
w = (0.7476 - 0.5940) × 800 = 123px
h = (0.0724 - 0.0007) × 450 = 32px

moving:true  →  🔴 빨간 네모 + 글로우
```

**끝!** 이게 초당 5~7번 반복된다.

---

## 3. 6프레임 연속 — 표로 한눈에

### 🚶 걸어가는 사람 (ObjectId 22020, 실제 로그)

| # | 시각 | cx | 기록장 상태 | 판정 |
|---|------|-----|-------------|------|
| 1 | 51.144 | 1589.0 | `[1589]` | 아직 6장 안 참 |
| 2 | 51.344 | 1605.5 | `[1589,1605]` | 대기 |
| 3 | 51.611 | 1645.0 | `[1589,1605,1645]` | 대기 |
| 4 | 51.744 | 1672.5 | `[…1672]` | 대기 |
| 5 | 52.011 | 1738.0 | `[…1738]` | 대기 (5장) |
| 6 | 52.211 | 1790.5 | `[1589…1790]` **6장!** | **비교 → 201.8px > 77.7px → 🎯 발사!** |
| 7 | 52.411 | 1899.5 | `[1605…1899]` (1589 버림) | 이미 moving=true → 조용 |

> 7번째부터는 **이미 `moving=true`** 라서 이벤트를 또 안 쏜다. 조용히 추적만.

### 🧍 서 있는 사람 (ObjectId 1981, 실제 로그)

| # | 시각 | cx | cy | 판정 |
|---|------|-----|-----|------|
| 1 | 05.065 | 987.5 | 909.5 | |
| 2 | 05.332 | 987.5 | 909.5 | |
| 3 | 05.465 | 987.5 | 909.5 | |
| 4 | 05.665 | 987.5 | 909.5 | |
| 5 | 05.865 | 987.5 | 909.5 | |
| 6 | 06.065 | 987.5 | 909.5 | 거리 = **0px** < 77.7px → **조용** ✅ |

**정지 객체는 이벤트가 안 나간다.** 우리가 원했던 바로 그것.

---

## 4. 자주 헷갈리는 것 Q&A

### Q1. WiseAI가 XML을 "저장"하고 있나?
**아니다.** 매 프레임 새로 만들어서 던지고 잊는다. 저장소가 아니라 **중계 방송**이다.
과거를 기억하는 건 오직 우리 앱의 `tracks_` 기록장뿐.

### Q2. 그럼 앱을 껐다 켜면?
기록장이 다 날아간다. 다시 6프레임 모아야 판정이 시작된다. (약 1.2초)

### Q3. XML이 초당 5장이면 하루에 몇 장?
`5 × 60 × 60 × 24 = 432,000장`. 그래서 **저장하면 안 되고 흘려보내야** 한다.
우리도 객체당 딱 6개 좌표만 들고 있고, 15프레임 안 보이면 지운다.

### Q4. `ObjectId` 는 계속 같은 번호인가?
같은 사람이 화면에 있는 동안은 유지된다. 하지만:
- 사람이 나갔다 다시 들어오면 **새 번호**
- 기둥에 가려졌다 나오면 **새 번호일 수도**
- 그래서 로그에 `22020`, `22032`, `22035` 처럼 번호가 계속 늘어난다

### Q5. 왜 `Human` 만 쓰나? `Face` 도 사람인데?
사람 1명 = `Human`+`Head`+`Face` **3개 객체**로 잡힌다.
전부 추적하면 한 명이 지나가는데 이벤트가 **3번** 터진다.
`Human`(몸통)만 = **사람 1명당 1번**. 깔끔.

### Q6. 채널이 왜 여러 개?
이 카메라(PNM-C16083RVQ)는 **렌즈가 여러 개인 다방향 카메라**.
렌즈마다 따로 AI가 돌아서 `ch0`, `ch1` … 이 각각 온다.
그래서 우리 코드의 모든 변수가 `[4]` 배열이다 → `tracks_[ch]`, `latest_[ch]`

### Q7. `latest_[ch]` 는 왜 매번 통째로 갈아치우나?
```cpp
latest_[ch].swap(dets);   // 이번 프레임 결과로 통째 교체
```
"지금 화면에 뭐가 있나"는 **최신 프레임이 곧 정답**이기 때문.
누적할 이유가 없다. (누적하는 건 `tracks_` 쪽)

### Q8. 브라우저 폴링(0.15초)과 XML(0.2초)이 안 맞는데?
상관없다. 브라우저는 그냥 **"지금 최신 박스 뭐야?"** 물어보고,
앱은 **가장 최근에 계산해둔 것**(`latest_`)을 준다. 서로 독립적으로 돈다.

### Q9. `MetadataManager` 는 우리 앱 안에 있나?
**아니다. 카메라 본체(`System`) 소속이다.** 우리가 만든 게 아니라 한화가 만들어둔 것.
같은 카메라 안이지만 **다른 컨테이너(회사)** 라서, 매니페스트로 연결선을 그어야 방송이 들어온다.
→ [1-1장](#1-1-metadatamanager-는-우리-앱-밖에-있다-) 참고

### Q10. `MetadataManager` 가 4개인가?
**그렇다. 채널(렌즈)마다 하나씩** — `MetadataManager_0` ~ `_3`.
`{Ch}` 템플릿 한 줄이 네 개를 다 구독해준다.
그래서 우리 코드 변수도 전부 `[4]` 배열이다 (`tracks_[4]`, `latest_[4]`, `tick_[4]`…).

### Q11. 저 ONVIF XML을 "메타데이터"라고 하면 되나?
**맞다.** 영상(그림) 자체가 아니라 **영상에 대한 설명 정보**라서 "메타(meta = ~에 대한)데이터".
`MetadataManager` 가 이걸 뿌리고, `output()` 으로 꺼내면 그 XML 문자열이다.

### Q12. 객체가 없으면 메타데이터를 안 보내나?
**아니다. 빈 프레임도 계속 보낸다.** (실제 로그로 확인됨)
```xml
<tt:Frame UtcTime="...:08.666Z">
  <tt:Scale x="0.000000" y="0.000000"/>   ← scale 0
</tt:Frame>                                 ← 객체 없음
```
`<tt:Object>` 를 못 찾으면 우리 루프가 그냥 끝나서 **아무 일도 안 일어난다.**
단, **빈 프레임은 `Scale` 이 0** 이라 `2 ÷ 0` 사고가 날 뻔했다 → 코드에 방어벽 있음.

### Q13. 메타데이터에 좌표 말고 다른 것도 오나?
**온다.** 같은 통로로 `<tt:Event>` 알림들(WiseAI ObjectDetection on/off, MotionAlarm, 앱 상태)이 섞여 온다.
우린 `<tt:Object ObjectId=` 가 있는 것만 처리하므로 **자동으로 걸러진다.**
→ 참고로 `ObjectDetection` 이벤트는 **"있다/없다"만** 주고 좌표가 없어서 움직임 판정에 못 쓴다.

---

## 5. 한 문장 정리

> **WiseAI는 "지금 여기 있다"를 초당 5번 외치고,
> 우리 앱은 그걸 6번 받아 적었다가 첫 번째와 여섯 번째를 비교해서
> "어 이 사람 움직였네!" 하고 소리친다.**

그게 전부다.

---

관련 문서: [BASICS.md](BASICS.md) (용어·개념) · [README.md](README.md) (구현 상세) · [FROM_SCRATCH.md](FROM_SCRATCH.md) (직접 만들기)
