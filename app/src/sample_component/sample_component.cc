#include "sample_component.h"

#include <dirent.h>                     // [④탐사] /lsdownload 디렉터리 나열
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/imgcodecs.hpp>        // cv::imread — 저장된 크롭을 OCR 입력으로 다시 읽음

#include "i_p_metadata_manager.h"       // IPMetadataManager::MetadataOutput
#include "dispatcher_serialize.h"       // SerializableString, OpenAppSerializable, JsonUtility
#include "i_log_manager.h"              // ILogManager remote debug message (DEBUG VIEWER)
#include "i_app_dispatcher.h"           // IAppDispatcher (HTTP / OpenAPI)
#include "i_eventstatus_cgi_dispatcher.h"  // [EventStatus] SUNAPI eventstatus.cgi 이벤트 타입
#include "i_p_open_platform_manager.h"  // IPOpenPlatformManager::eAppSnapshotJpeg
#include "life_cycle_manager_openapp.h" // LifeCycleManagerOpenApp::eInformAppInfo (app_id)

#include "config.h"                     // [1단계] 설정값 단일 출처 (경로/링크기/채널수)
#include "io/debug_viewer_sink.h"       // [1단계] 이벤트 출구(sink)
#include "io/disk_log_sink.h"           // [4단계] 확장성 증명 — 새 sink (도메인 무수정)
#include "core/metadata_parser.h"       // [2단계] XML 파싱 격리 (SDK 무관 객체 구조)

// 채널별 상태 배열이 [4]로 하드코딩되어 있으므로, 설정값이 어긋나면 컴파일 실패로 잡는다.
static_assert(cfg::kChannels == 4, "channel-indexed arrays are sized [4]; keep cfg::kChannels in sync");


namespace {
// (움직임 튜닝 상수는 [3단계]에서 config.h(cfg::)로 이동 → MotionTracker 가 사용)

// 디버그 뷰어 로그 강조: kAnsiLogs=false 면 빈 문자열(색 없음)로 대체.
inline const char* Hl(const char* code) { return cfg::kAnsiLogs ? code : ""; }

// 판 색 코드 → 사람이 읽는 이름 (EV 판정엔 안 씀 — 데이터 확인용 눈요기).
//   wh 흰색 · ye 노란(영업용) · gr 연두(친환경 법인) · bl 파랑(전기·수소 개인)
inline const char* ColorName(const std::string& c) {
  if (c == "wh") return "white";
  if (c == "ye") return "yellow";
  if (c == "gr") return "green";
  if (c == "bl") return "blue";
  return "unknown";
}

// [EV 실조회] ev_cache_ 판정값의 특수 상태 (1=EV, 0=아님 은 EvClient 규약)
constexpr int kEvNone = -9;     // 캐시에 없음 — 첫 조회 필요
constexpr int kEvPending = -2;  // 조회 요청됨 — 응답 대기중

// 현재 시각(ms). 움직임 3초 유지 타이머용.
uint64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// URL 퍼센트 인코딩 디코드 (/isev?plate=... 의 한글 UTF-8 처리)
std::string UrlDecode(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size()) {
      auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      int h = hex(s[i + 1]), l = hex(s[i + 2]);
      if (h >= 0 && l >= 0) { out += (char)(h * 16 + l); i += 2; continue; }
    }
    out += (s[i] == '+') ? ' ' : s[i];
  }
  return out;
}

// (XML 파싱 헬퍼 FindAttr/Trim/ExtractPlate 는 [2단계]에서 core/metadata_parser.h 로 이동)

}  // namespace

SampleComponent::SampleComponent() : SampleComponent(_SampleComponent_Id, "SampleComponent") {}

SampleComponent::SampleComponent(ClassID id, const char* name) : Component(id, name) {}

SampleComponent::~SampleComponent() {
  ev_client_.Stop();                // [EV 실조회] 워커 스레드 조인 (sink 보다 먼저)
  for (auto* s : sinks_) delete s;  // [1단계] 등록된 출구 정리
}

bool SampleComponent::Initialize() {
  setvbuf(stdout, NULL, _IONBF, 0);  // printf 버퍼링 끄기 → 디버그 로그 CGI에 즉시 표시

  // [1단계] 이벤트 출구 등록: 지금은 DEBUG VIEWER 로 보내는 sink 하나뿐(동작 불변).
  //   실제 SDK 전송(SendTargetEvents)을 람다로 주입 → sink 는 SDK 를 몰라도 됨.
  sinks_.push_back(new DebugViewerSink([this](const std::string& m) {
    SendTargetEvents(ILogManager::remote_debug_message_group,
                     static_cast<int32_t>(ILogManager::EEvent::eRemoteDebugMessage), 0,
                     new ("") Platform_Std_Refine::SerializableString(m.c_str()));
  }));
  // [4단계] 확장성 증명: 새 출구(파일 이력)를 이 한 줄로 추가 — 도메인 코드는 그대로.
  sinks_.push_back(new DiskLogSink());

  // [EV 실조회] 워커 시작. 이 콜백은 워커 스레드에서 불리므로 큐에만 적재하고,
  //   이벤트 발행은 메타데이터 스레드의 DrainEvResults() 가 맡는다.
  ev_client_.Start([this](const EvClient::Result& r) {
    std::lock_guard<std::mutex> lk(ev_mtx_);
    ev_results_.push_back(r);
  });

  parking_.SetPersistPath(cfg::kParkFile);  // [주차] 저장된 구역 로드(재부팅 후 유지)
  park_tune_.Load("../storage/parking_tune.txt");  // [튜닝] 저장된 런타임 파라미터
  parking_.SetTune(&park_tune_);

  RegisterURI();                     // /detections HTTP 엔드포인트 등록

  // 번호판 OCR 모델 로드(tinyLPR 단독 — Multi-line 은 기여 0 실측으로 제거).
  // 실패해도 앱은 계속 동작(크롭 저장은 그대로).
  plate_ocr_ready_ = plate_ocr_.Load(cfg::kKrLprModel, cfg::kKrLprLabels);
  printf("[object_detect] plate OCR model %s\n", plate_ocr_ready_ ? "loaded" : "FAILED to load");

  // 등록차량 DB 로드 — 파일 없으면 순수 인식 모드로 동작
  plate_db_ready_ = cfg::kPlateDb && plate_db_.Load(cfg::kPlateDbFile);
  printf("[object_detect] plate DB %s (%d plates)\n",
         plate_db_ready_ ? "loaded" : "off", plate_db_ready_ ? plate_db_.size() : 0);

  printf("[object_detect] app started, waiting for metadata...\n");
  // DEBUG VIEWER 에도 시작 알림 (연결 확인용)
  SendTargetEvents(ILogManager::remote_debug_message_group,
                   static_cast<int32_t>(ILogManager::EEvent::eRemoteDebugMessage), 0,
                   new ("") Platform_Std_Refine::SerializableString("[object_detect] app started"));
  return Component::Initialize();
}

bool SampleComponent::ProcessAEvent(Event* event) {
  switch (event->GetType()) {
    case static_cast<int32_t>(IPMetadataManager::EEventType::eMetadataRequest):
      HandleMetadataEvent(event);
      break;
    case static_cast<int32_t>(IAppDispatcher::EEventType::eHttpRequest):
      HandleHttpRequest(event);
      break;
    // [EventStatus] 펌웨어 EventstatusCGIDispatcher 의 요청 (OpenEventDispatcher 가 중계).
    case static_cast<int32_t>(I_EventstatusCGIDispatcher::EEventType::eEventstatusSchema):
      SendParkingEventSchema(event);
      break;
    case static_cast<int32_t>(I_EventstatusCGIDispatcher::EEventType::eMetadataSchema):
      SendParkingMetaSchema(event);
      break;
    case static_cast<int32_t>(I_EventstatusCGIDispatcher::EEventType::eEventStatusCheck):
      // 초기 상태 확인 요청 — 칸별 ParkingOccupied 로 응답 (단일 이벤트 체제)
      for (int c = 0; c < cfg::kChannels; c++) {
        int idx = 0;
        for (const auto& s : parking_.Snapshot(c))
          NotifyOccupied(c, ++idx, s.id, s.occupied, true);
      }
      break;
    case static_cast<int32_t>(LifeCycleManagerOpenApp::EEventType::eInformAppInfo): {
      auto blob = event->GetBlobArgument();
      auto base_object = blob.GetBaseObject();
      if (base_object) {
        auto str = *(static_cast<String*>(base_object));
        JsonUtility::JsonDocument doc;
        doc.Parse(str.c_str());
        if (doc.HasMember("AppId") && doc["AppId"].IsString()) app_id_ = doc["AppId"].GetString();
      }
      break;
    }
    default:
      Component::ProcessAEvent(event);
      break;
  }
  return true;
}

void SampleComponent::HandleMetadataEvent(Event* event) {
  if (event == nullptr || event->IsReply()) return;

  DrainEvResults();  // [EV 실조회] 도착한 판정 결과를 이 스레드에서 발행

  auto attachment = event->GetAttachment<IPMetadataManager::MetadataOutput>();
  if (!attachment) return;

  int ch = attachment->channel();
  std::string xml(attachment->output().c_str());

  ProcessObjects(ch, xml);
}

// [1단계] 이벤트를 등록된 모든 출구(sink)로 뿌리기만 한다. 목적지는 sink 가 결정.
void SampleComponent::EmitEvent(int channel, const std::string& msg) {
  for (auto* s : sinks_) s->OnEvent(channel, msg);
}

// [EV 판정] ★FINAL 발급 시 {번호, 판 색, 시각}을 이력에 기록 — /isev 조회용 —
//   하고 그 자리에서 전기차 여부를 판정해 디버그 뷰어에 알린다.
//   색은 그 채널 마지막 good-shot 판독의 판 색 분류(버스트 승리여도 같은 차의 색).
//   판정: 등록차는 DB 플래그(등록 시점에 ev.or.kr 저공해 1종 조회로 확정된 ",ev")가
//   정답, 미등록차는 ev.or.kr 실조회로 확정. 판 색은 EV 판정에 쓰지 않고(법인 전기차는
//   연두색이라 "파랑=EV"가 틀림) 로그에 참고용으로만 병기한다.
void SampleComponent::RecordFinal(int ch, const std::string& text, uint64_t now_ms, long oid) {
  const std::string color = last_plate_ocr_[ch].color;
  finals_log_.push_back({text, color, now_ms});
  while (finals_log_.size() > 64) finals_log_.pop_front();

  // [주차 위치배정 준비] 이 번호판(oid)의 마지막 중심좌표 — 그 좌표가 든 칸에 배정.
  double px = -1.0, py = -1.0;
  {
    auto pp = plate_pos_[ch].find(oid);
    if (pp != plate_pos_[ch].end()) { px = pp->second.first; py = pp->second.second; }
  }

  const char* pc = color.empty() ? "?" : color.c_str();

  // [눈요기] 이 차를 무슨 색 판으로 봤나 — 판정엔 안 쓰고 데이터 확인용으로만 찍는다.
  char cm[160];
  snprintf(cm, sizeof(cm), "%s🎨 COLOR ch%d \"%s\" -> %s (%s)%s",
           Hl(cfg::kAnsiInfo), ch, text.c_str(), ColorName(color), pc, Hl(cfg::kAnsiReset));
  EmitEvent(ch, cm);

  std::string canon;
  bool registered = plate_db_ready_ && plate_db_.Match(text, &canon) == 1;
  char m[320];

  // [주차] 증거 크롭 URL (Pi 가 카메라 호스트 앞에 붙여 사용). 방금 저장분 슬롯.
  char ev_url[80];
  snprintf(ev_url, sizeof(ev_url), "/opensdk/object_detect/plate?n=%d", plate_store_.last_slot(ch));

  if (registered) {  // 등록차 — DB 플래그(등록 시점에 ev.or.kr 로 확정된 값)가 정답
    bool ev = plate_db_.IsEv(canon);
    // [주차] 위치 배정 — 좌표 없으면(-1) AssignAt 폴백이 "번호 없는 진행중 칸"을 잡는다.
    { std::string pk = parking_.AssignAt(ch, px, py, text, ev ? 1 : 0, "registered", ev_url, now_ms);
      if (!pk.empty()) EmitEvent(ch, pk); }
    CheckParkingEvent(ch);               // [EventStatus] 위반 확정 시 통지
    snprintf(m, sizeof(m), "%s⚡EV ch%d \"%s\" -> %s (registered, DB ev=%s, color %s)%s",
             Hl(ev ? cfg::kAnsiEv : cfg::kAnsiDim), ch, text.c_str(),
             ev ? "★EV★" : "non-EV", ev ? "Y" : "N", pc, Hl(cfg::kAnsiReset));
    EmitEvent(ch, m);
    return;
  }

  if (!cfg::kEvLiveLookup) {  // 실조회 꺼짐 — 판정 불가 (색 추정 안 함)
    { std::string pk = parking_.AssignAt(ch, px, py, text, -1, "", ev_url, now_ms,
                                         /*replace_ok=*/false);
      if (!pk.empty()) EmitEvent(ch, pk); }  // [주차] 번호만, EV 판정불가 (미등록 — 교체 금지)
    CheckParkingEvent(ch);  // [EventStatus] (unknown 은 위반 아님 — 대개 no-op)
    //   판 색으로 EV 를 추정하지 않는다: 파란판만 전기차가 아니다. 법인 전기차는
    //   연두색(친환경 법인판)이라 "파랑=EV" 규칙은 법인차를 통째로 놓친다 (07-30).
    //   미등록차의 정답은 오직 등록 시점 ev.or.kr 조회. 실조회 꺼진 현장은 판정 보류.
    snprintf(m, sizeof(m), "%s⚡EV ch%d \"%s\" -> unknown (unregistered, lookup off, color %s)%s",
             Hl(cfg::kAnsiDim), ch, text.c_str(), pc, Hl(cfg::kAnsiReset));
    EmitEvent(ch, m);
    return;
  }

  // 미등록차 — 카메라가 ev.or.kr 을 직접 조회. 같은 번호는 캐시로 1회만.
  int cached = kEvNone;
  std::string cdetail;
  {
    std::lock_guard<std::mutex> lk(ev_mtx_);
    auto it = ev_cache_.find(text);
    if (it != ev_cache_.end()) {
      cached = it->second.first;
      cdetail = it->second.second;
    } else {
      ev_cache_[text] = {kEvPending, ""};  // 조회중 마크 — 중복 요청 방지
    }
  }
  // [주차] 위치 배정 + 현재 EV 상태 반영 (조회중이면 unknown, 캐시에 있으면 확정치).
  //   미확정(unknown)은 나중에 DrainEvResults 가 OnEvResult 로 채운다.
  int evs = cached == 1 ? 1 : (cached == 0 ? 0 : -1);
  { std::string pk = parking_.AssignAt(ch, px, py, text, evs, "lookup", ev_url, now_ms,
                                       /*replace_ok=*/false);   // 미등록 판독 — 빈 칸만
    if (!pk.empty()) EmitEvent(ch, pk); }
  CheckParkingEvent(ch);  // [EventStatus] 캐시로 즉시 위반 확정된 경우 통지

  if (cached == kEvNone) {
    ev_client_.Request(ch, text);
    snprintf(m, sizeof(m), "%s⚡EV ch%d \"%s\" -> ev.or.kr live lookup... (color %s)%s",
             Hl(cfg::kAnsiInfo), ch, text.c_str(), pc, Hl(cfg::kAnsiReset));
  } else if (cached == kEvPending) {
    snprintf(m, sizeof(m), "%s⚡EV ch%d \"%s\" -> awaiting lookup response...%s",
             Hl(cfg::kAnsiInfo), ch, text.c_str(), Hl(cfg::kAnsiReset));
  } else {
    bool ev = cached == 1;
    snprintf(m, sizeof(m), "%s⚡EV ch%d \"%s\" -> %s (lookup cache: %s, color %s)%s",
             Hl(ev ? cfg::kAnsiEv : cfg::kAnsiDim), ch, text.c_str(),
             ev ? "★EV★" : "non-EV", cdetail.c_str(), pc, Hl(cfg::kAnsiReset));
  }
  EmitEvent(ch, m);
}

// [EV 실조회] 워커가 넣어둔 조회 결과를 메타데이터 스레드에서 회수해 발행.
//   성공(1/0)은 캐시에 확정 기록, 실패(-1)는 캐시에서 지워 다음 FINAL 때 재시도.
void SampleComponent::DrainEvResults() {
  std::deque<EvClient::Result> rs;
  {
    std::lock_guard<std::mutex> lk(ev_mtx_);
    if (ev_results_.empty()) return;
    rs.swap(ev_results_);
    for (const auto& r : rs) {
      if (r.verdict >= 0) ev_cache_[r.plate] = {r.verdict, r.detail};
      else ev_cache_.erase(r.plate);
    }
  }
  for (const auto& r : rs) {
    // [주차] 실조회 결과가 그 번호로 주차된 구역이 있으면 EV·위반 갱신 (verdict -1 은 미확정).
    if (r.verdict >= 0)
      for (const auto& pk : parking_.OnEvResult(r.plate, r.verdict)) EmitEvent(r.ch, pk);
    char m[384];
    if (r.verdict == 1)
      snprintf(m, sizeof(m), "%s⚡EV ch%d \"%s\" -> ★EV★ (ev.or.kr lookup: %s)%s",
               Hl(cfg::kAnsiEv), r.ch, r.plate.c_str(), r.detail.c_str(),
               Hl(cfg::kAnsiReset));
    else if (r.verdict == 0)
      snprintf(m, sizeof(m), "%s⚡EV ch%d \"%s\" -> non-EV (ev.or.kr lookup: %s)%s",
               Hl(cfg::kAnsiDim), r.ch, r.plate.c_str(), r.detail.c_str(),
               Hl(cfg::kAnsiReset));
    else
      snprintf(m, sizeof(m), "%s⚡EV ch%d \"%s\" -> unknown (%s) — retry on next final%s",
               Hl(cfg::kAnsiWarn), r.ch, r.plate.c_str(), r.detail.c_str(),
               Hl(cfg::kAnsiReset));
    EmitEvent(r.ch, m);
    CheckParkingEvent(r.ch);  // [EventStatus] 실조회로 위반이 확정/해제됐으면 통지
  }
}

// ===== [EventStatus] 주차위반 → 카메라 네이티브 SUNAPI 이벤트 ================
//   dynamic_event 샘플 프로토콜 그대로: 스키마 2종(JSON+TEXT / ONVIF XML) 등록 후,
//   위반상태가 바뀔 때 eEventStatusChanged 를 OpenEventDispatcher 로 발송.
//   확인: eventstatus.cgi?msubmenu=eventstatusschema&action=view 에 이벤트 등장,
//         eventstatus.cgi?msubmenu=eventstatus&action=monitordiff 로 실시간 수신.
namespace {
constexpr const char* kParkEvName = "OpenSDK.object_detect.ParkingViolation";
constexpr const char* kParkEvName2 = "OpenSDK.object_detect.ParkingOccupied";
}

void SampleComponent::SendParkingEventSchema(Event* event) {
  // JSON 스키마(신형 장비) + TEXT 스키마(구형 NVR 텍스트 프로토콜)
  static const char* kSchema =
      "{"
      "\"JSON\": {"
        "\"type\": \"object\","
        "\"properties\": {"
          "\"Time\": {\"type\": \"string\"},"
          "\"EventName\": {\"enum\": [\"OpenSDK.object_detect.ParkingViolation\"]},"
          "\"Source\": {"
            "\"type\": \"object\","
            "\"properties\": {"
              "\"Channel\": {\"type\": \"number\"},"
              "\"AppName\": {\"type\": \"string\"},"
              "\"AppEvent\": {\"type\": \"string\"},"
              "\"AppID\": {\"type\": \"string\"},"
              "\"Type\": {\"enum\": [\"Event\"]},"
              "\"RuleIndex\": {\"type\": \"number\"},"
              "\"VideoSourceToken\": {\"type\": \"string\"},"
              "\"RuleName\": {\"type\": \"string\"}"
            "}"
          "},"
          "\"Data\": {"
            "\"type\": \"object\","
            "\"properties\": {"
              "\"State\": {\"type\": \"boolean\"},"
              "\"Occupied\": {\"type\": \"boolean\"},"
              "\"Plate\": {\"type\": \"string\"},"
              "\"EV\": {\"type\": \"string\"}"
            "}"
          "}"
        "}"
      "},"
      "\"TEXT\": {"
        "\"SCHEME\": "
        "\"Name=OpenSDK.object_detect.ParkingViolation\\n"
        "Schema.1.Name=Channel.<int>.OpenSDK.object_detect.ParkingViolation\\n"
        "Schema.1.Value=<boolean>\\n"
        "Schema.2.Name=Channel.<int>.OpenSDK.object_detect.ParkingViolation.<int>.VideoSourceToken\\n"
        "Schema.2.Value=<string>\\n"
        "Schema.3.Name=Channel.<int>.OpenSDK.object_detect.ParkingViolation.<int>.RuleName\\n"
        "Schema.3.Value=<string>\\n"
        "Schema.4.Name=Channel.<int>.OpenSDK.object_detect.ParkingViolation.<int>.State\\n"
        "Schema.4.Value=<boolean>\\n"
        "Schema.5.Name=Channel.<int>.OpenSDK.object_detect.ParkingViolation.<int>.Plate\\n"
        "Schema.5.Value=<string>\\n"
        "Schema.6.Name=Channel.<int>.OpenSDK.object_detect.ParkingViolation.<int>.EV\\n"
        "Schema.6.Value=<string>\\n"
        "Schema.7.Name=Channel.<int>.OpenSDK.object_detect.ParkingViolation.<int>.Occupied\\n"
        "Schema.7.Value=<boolean>\""
      "}"
      "}";
  // [이벤트 2호] ParkingOccupied — 채널+칸별(룰 N=칸 순번) 점유 불리언.
  //   한 앱 복수 이벤트는 WiseAI 가 3개 등록한 실증이 있음 — 스키마 응답을 장수만큼 발송.
  static const char* kSchema2 =
      "{"
      "\"JSON\": {"
        "\"type\": \"object\","
        "\"properties\": {"
          "\"Time\": {\"type\": \"string\"},"
          "\"EventName\": {\"enum\": [\"OpenSDK.object_detect.ParkingOccupied\"]},"
          "\"Source\": {"
            "\"type\": \"object\","
            "\"properties\": {"
              "\"Channel\": {\"type\": \"number\"},"
              "\"AppName\": {\"type\": \"string\"},"
              "\"AppEvent\": {\"type\": \"string\"},"
              "\"AppID\": {\"type\": \"string\"},"
              "\"Type\": {\"enum\": [\"Event\"]},"
              "\"RuleIndex\": {\"type\": \"number\"},"
              "\"VideoSourceToken\": {\"type\": \"string\"},"
              "\"RuleName\": {\"type\": \"string\"}"
            "}"
          "},"
          "\"Data\": {"
            "\"type\": \"object\","
            "\"properties\": {\"State\": {\"type\": \"boolean\"}}"
          "}"
        "}"
      "},"
      "\"TEXT\": {"
        "\"SCHEME\": "
        "\"Name=OpenSDK.object_detect.ParkingOccupied\\n"
        "Schema.1.Name=Channel.<int>.OpenSDK.object_detect.ParkingOccupied\\n"
        "Schema.1.Value=<boolean>\\n"
        "Schema.2.Name=Channel.<int>.OpenSDK.object_detect.ParkingOccupied.<int>.State\\n"
        "Schema.2.Value=<boolean>\""
      "}"
      "}";
  (void)kSchema;  // [단일화] ParkingOccupied 만 등록 — 위반 판단은 XML violation 필드 (08-04)
  try {
    SendNoReplyEvent("OpenEventDispatcher", event->GetType(), 0, new ("") String(kSchema2));
  } catch (...) {}
}

void SampleComponent::SendParkingMetaSchema(Event* event) {
  // ONVIF 토픽 스키마 — 메타데이터 스트림/이벤트 토픽 목록 등록용
  static const char* kMetaXml =
      "<tns1:OpenApp><object_detect>"
      "<ParkingViolation wstop:topic=\\\"true\\\">"
      "<tt:MessageDescription IsProperty=\\\"true\\\">"
      "<tt:Source>"
      "<tt:SimpleItemDescription Name=\\\"VideoSourceToken\\\" Type=\\\"tt:ReferenceToken\\\"/>"
      "<tt:SimpleItemDescription Name=\\\"RuleName\\\" Type=\\\"xsd:string\\\"/>"
      "</tt:Source>"
      "<tt:Data>"
      "<tt:SimpleItemDescription Name=\\\"State\\\" Type=\\\"xsd:boolean\\\"/>"
      "<tt:SimpleItemDescription Name=\\\"Occupied\\\" Type=\\\"xsd:boolean\\\"/>"
      "<tt:SimpleItemDescription Name=\\\"Plate\\\" Type=\\\"xsd:string\\\"/>"
      "<tt:SimpleItemDescription Name=\\\"EV\\\" Type=\\\"xsd:string\\\"/>"
      "</tt:Data>"
      "</tt:MessageDescription></ParkingViolation></object_detect></tns1:OpenApp>";
  std::string body = std::string("{")
      + "\"PROPRIETARY\": {"
        "\"EventName\": \"" + kParkEvName + "\","
        "\"EventTopic\": \"tns1:OpenApp/object_detect/ParkingViolation\","
        "\"EventSchema\": \"" + kMetaXml + "\"},"
      + "\"ONVIF\": {"
        "\"EventName\": \"" + kParkEvName + "\","
        "\"EventTopic\": \"tns1:OpenApp/object_detect/ParkingViolation\","
        "\"EventSchema\": \"" + kMetaXml + "\"}"
      + "}";
  // [이벤트 2호] ParkingOccupied 토픽
  static const char* kMetaXml2 =
      "<tns1:OpenApp><object_detect>"
      "<ParkingOccupied wstop:topic=\\\"true\\\">"
      "<tt:MessageDescription IsProperty=\\\"true\\\">"
      "<tt:Source>"
      "<tt:SimpleItemDescription Name=\\\"VideoSourceToken\\\" Type=\\\"tt:ReferenceToken\\\"/>"
      "<tt:SimpleItemDescription Name=\\\"RuleName\\\" Type=\\\"xsd:string\\\"/>"
      "</tt:Source>"
      "<tt:Data>"
      "<tt:SimpleItemDescription Name=\\\"State\\\" Type=\\\"xsd:boolean\\\"/>"
      "</tt:Data>"
      "</tt:MessageDescription></ParkingOccupied></object_detect></tns1:OpenApp>";
  std::string body2 = std::string("{")
      + "\"PROPRIETARY\": {"
        "\"EventName\": \"" + kParkEvName2 + "\","
        "\"EventTopic\": \"tns1:OpenApp/object_detect/ParkingOccupied\","
        "\"EventSchema\": \"" + kMetaXml2 + "\"},"
      + "\"ONVIF\": {"
        "\"EventName\": \"" + kParkEvName2 + "\","
        "\"EventTopic\": \"tns1:OpenApp/object_detect/ParkingOccupied\","
        "\"EventSchema\": \"" + kMetaXml2 + "\"}"
      + "}";
  (void)body;  // [단일화] ParkingOccupied 토픽만 등록 (08-04)
  try {
    SendNoReplyEvent("OpenEventDispatcher", event->GetType(), 0, new ("") String(body2.c_str()));
  } catch (...) {}
}

// [벨 1호 — 위반 알람] 채널의 위반 여부를 ChannelEvent JSON 으로 발송.
//   (점유/출차 벨은 2호 ParkingOccupied 가 칸별로 담당 — 08-04 이벤트 분리)
void SampleComponent::NotifyParkingEvent(int ch, bool check, int force_state) {
  (void)force_state;
  std::string ids, plates;
  bool viol = parking_.ViolationState(ch, &ids, &plates);
  if (ids.empty()) ids = "-";
  std::string ts = I_EventstatusCGIDispatcher::GetCurrentTimeStamp();
  const char* app = app_id_.empty() ? "object_detect" : app_id_.c_str();
  char buf[1024];
  snprintf(buf, sizeof(buf),
           "{\"ChannelEvent\":{\"Channel\":%d,\"State\":true,\"Time\":\"%s\","
           "\"EventName\":\"%s\","
           "\"Source\":{\"Channel\":%d,\"AppName\":\"object_detect\",\"AppID\":\"%s\","
           "\"AppEvent\":\"ParkingViolation\",\"Type\":\"Event\",\"RuleIndex\":1,"
           "\"VideoSourceToken\":\"vs-%d\",\"RuleName\":\"%s\"},"
           "\"Data\":{\"State\":%s,\"Plate\":\"%s\",\"EV\":\"%s\"}}}",
           ch, ts.c_str(), kParkEvName, ch, app, ch, ids.c_str(),
           viol ? "true" : "false", plates.c_str(), viol ? "no" : "");
  try {
    SendNoReplyEvent("OpenEventDispatcher",
                     static_cast<int32_t>(check
                         ? I_EventstatusCGIDispatcher::EEventType::eEventStatusCheck
                         : I_EventstatusCGIDispatcher::EEventType::eEventStatusChanged),
                     0, new ("") String(buf));
  } catch (...) {}
}

// [벨 2호 — 칸별 점유] ParkingOccupied 이벤트. RuleIndex = 칸 순번(1부터, XML 순서와
//   동일), State = 점유. 주차/출차 순간 그 칸 룰이 뒤집히고, 점유 중 내용 갱신(번호·EV
//   확정)은 호출측이 펄스(False→True)로 라인을 강제해 Pi 의 XML 재열람을 유발한다.
void SampleComponent::NotifyOccupied(int ch, int rule_idx, const std::string& space_id,
                                     bool occ, bool check) {
  std::string ts = I_EventstatusCGIDispatcher::GetCurrentTimeStamp();
  const char* app = app_id_.empty() ? "object_detect" : app_id_.c_str();
  char buf[768];
  snprintf(buf, sizeof(buf),
           "{\"ChannelEvent\":{\"Channel\":%d,\"State\":true,\"Time\":\"%s\","
           "\"EventName\":\"%s\","
           "\"Source\":{\"Channel\":%d,\"AppName\":\"object_detect\",\"AppID\":\"%s\","
           "\"AppEvent\":\"ParkingOccupied\",\"Type\":\"Event\",\"RuleIndex\":%d,"
           "\"VideoSourceToken\":\"vs-%d\",\"RuleName\":\"%s\"},"
           "\"Data\":{\"State\":%s}}}",
           ch, ts.c_str(), kParkEvName2, ch, app, rule_idx, ch, space_id.c_str(),
           occ ? "true" : "false");
  try {
    SendNoReplyEvent("OpenEventDispatcher",
                     static_cast<int32_t>(check
                         ? I_EventstatusCGIDispatcher::EEventType::eEventStatusCheck
                         : I_EventstatusCGIDispatcher::EEventType::eEventStatusChanged),
                     0, new ("") String(buf));
  } catch (...) {}
}

// [EventStatus 칸별 상태] 칸 하나를 하위 룰(RuleIndex 2..)로 통지.
//   State=그 칸의 위반, Occupied=점유, Plate/EV=판정 내용. RuleName=칸 id.
void SampleComponent::NotifyParkingSpace(int ch, int rule_idx, const ParkingZone::Snap& s,
                                         bool check) {
  std::string ts = I_EventstatusCGIDispatcher::GetCurrentTimeStamp();
  const char* app = app_id_.empty() ? "object_detect" : app_id_.c_str();
  const char* evs = s.ev == 1 ? "yes" : (s.ev == 0 ? "no" : "unknown");
  char buf[1024];
  snprintf(buf, sizeof(buf),
           "{\"ChannelEvent\":{\"Channel\":%d,\"State\":true,\"Time\":\"%s\","
           "\"EventName\":\"%s\","
           "\"Source\":{\"Channel\":%d,\"AppName\":\"object_detect\",\"AppID\":\"%s\","
           "\"AppEvent\":\"ParkingViolation\",\"Type\":\"Event\",\"RuleIndex\":%d,"
           "\"VideoSourceToken\":\"vs-%d\",\"RuleName\":\"%s\"},"
           "\"Data\":{\"State\":%s,\"Occupied\":%s,\"Plate\":\"%s\",\"EV\":\"%s\"}}}",
           ch, ts.c_str(), kParkEvName, ch, app, rule_idx, ch, s.id.c_str(),
           s.violation ? "true" : "false", s.occupied ? "true" : "false",
           s.plate.c_str(), s.occupied ? evs : "");
  try {
    SendNoReplyEvent("OpenEventDispatcher",
                     static_cast<int32_t>(check
                         ? I_EventstatusCGIDispatcher::EEventType::eEventStatusCheck
                         : I_EventstatusCGIDispatcher::EEventType::eEventStatusChanged),
                     0, new ("") String(buf));
  } catch (...) {}
}

// 칸별 전이 감지 → ParkingOccupied 통지. 주차/출차 = 그 칸 룰 값 변화,
//   점유 중 내용 갱신(번호·EV 확정) = 그 칸 룰 펄스(False→True). 반환: 변화 유무.
bool SampleComponent::CheckParkingStatus(int ch) {
  bool any_changed = false;
  auto snaps = parking_.Snapshot(ch);
  int idx = 0;  // ParkingOccupied 의 rule index = 칸 순번 (1부터, XML space 순서)
  for (const auto& s : snaps) {
    ++idx;
    auto it = park_snap_sent_.find(s.id);
    bool known = it != park_snap_sent_.end();
    if (known && it->second == s) continue;  // 변화 없음
    bool occ_changed = !known || it->second.occupied != s.occupied;
    park_snap_sent_[s.id] = s;
    any_changed = true;
    if (occ_changed) {
      NotifyOccupied(ch, idx, s.id, s.occupied, false);          // 주차/출차 — 값 전이
    } else if (s.occupied) {
      NotifyOccupied(ch, idx, s.id, false, false);               // 내용 갱신 — 펄스
      NotifyOccupied(ch, idx, s.id, true, false);
    }
    char m[224];
    snprintf(m, sizeof(m), "%s📡 Occupied.%d %s occupied=%d plate=%s ev=%s viol=%d%s%s",
             Hl(cfg::kAnsiDim), idx, s.id.c_str(), s.occupied ? 1 : 0,
             s.plate.empty() ? "-" : s.plate.c_str(),
             s.ev == 1 ? "yes" : (s.ev == 0 ? "no" : "?"), s.violation ? 1 : 0,
             occ_changed ? "" : (s.occupied ? " (pulse)" : " (no bell)"),
             Hl(cfg::kAnsiReset));
    EmitEvent(ch, m);
  }
  return any_changed;
}

// 주차 이벤트 트리거 지점 — 칸별 ParkingOccupied 통지 + 위반 전이 디버거 표기.
//   [단일화] EventStatus 는 ParkingOccupied 하나만 — 위반 판단은 XML violation 필드가
//   담당하므로 별도 벨을 안 울린다 (08-04 확정). 위반 전이는 디버거에만 남긴다.
void SampleComponent::CheckParkingEvent(int ch) {
  CheckParkingStatus(ch);
  bool viol = parking_.ViolationState(ch);
  if (viol == park_evt_state_[ch]) return;
  park_evt_state_[ch] = viol;
  char m[128];
  snprintf(m, sizeof(m), "%s⛔ violation ch%d -> %s (XML violation field)%s",
           Hl(viol ? cfg::kAnsiWarn : cfg::kAnsiDim), ch,
           viol ? "TRUE" : "false", Hl(cfg::kAnsiReset));
  EmitEvent(ch, m);
}

// [완전 리셋] 출차 순간 그 채널의 판독 진행상태 전부 폐기.
//   칸(Space)은 ResetLive 로 비워지지만, 판독 파이프라인(투표함·추적·스택·프레임세대)이
//   남아있으면 이전 차의 표가 다음 차 후보 시점에 개표되어 옛 번호가 꽂힌다 (08-04 실측:
//   흰 차 49허5678 출차 후 빨간 차 주차에 49허5678 배정). 채널 단위 전부 비운다.
void SampleComponent::PurgePlateState(int ch) {
  uint64_t now_g = NowMs();
  for (auto& kv : plate_seen_[ch]) {
    int n; double c; bool p;
    plate_vote_.Finalize(ch, kv.first, &n, &c, &p);  // 투표함 폐기 (결과 버림)
    // [유령 명부] 나간 차의 번호판 id — WiseAI 가 얼어붙은 레코드를 한동안 반복
    //   송신하므로, 이 id들의 번호판 기반 후보만 15초 차단 (새 id 는 즉시 통과 —
    //   "출차 직후 바로 다음 차" 시나리오를 시간 차단으로 막지 않는다. 08-06).
    ghost_plate_[ch][kv.first] = now_g;
  }
  plate_seen_[ch].clear();
  plate_done_[ch].clear();
  stack_acc_[ch].clear();
  plate_pos_[ch].clear();
  burst_ver_[ch].clear();
  pending_ocr_slot_[ch] = -1;
  futile_slot_[ch] = -1;
  plate_store_.ForgetRefs(ch);  // 다음 차의 크롭을 깨끗한 상태에서 수신
  purge_ms_[ch] = NowMs();  // [세션 경계] 이 시각 이전의 저장 크롭은 재판독 금지
}

// 방금 PlateStore 가 저장한 cap_<slot>.jpg 를 다시 읽어 번호판 숫자를 인식.
//   PC(ocr_lab)에서 검증한 tinyLPR+Multi-line 신뢰도 병합을 그대로 사용.
//   디버그 뷰어에 단계별 지연시간(load/tiny/multi/total)까지 찍어 병목을 드러낸다.
void SampleComponent::RecognizePlate(int ch, int slot) {
  if (!plate_ocr_ready_) {
    EmitEvent(ch, "OCR skipped: models not loaded");
    return;
  }
  if (ch < 0 || ch >= cfg::kChannels || slot < 0) return;
  // 이미 즉시확정된 차의 늦은 good-shot(재저장) — 판독 불필요 (이중판정 방지)
  if (plate_done_[ch].count(plate_store_.last_oid(ch))) return;
  // [헛수고 차단] 이 슬롯은 "이미 배정된 번호"로 판명난 크롭 — 재판독해도 얻을 게 없다
  if (slot == futile_slot_[ch]) return;
  // [판독 게이트 — 주차 전용 모드] "읽을 일이 있을 때"만: 후보 진행중 or 번호/EV
  //   미확정 점유칸. 구역 없는 채널 포함 그 외 전부 침묵 (08-04 B안 확정).
  if (!parking_.NeedsRead(ch)) {
    pending_ocr_slot_[ch] = slot;  // [회수 예약] 읽을 일이 생기는 순간 꺼내 읽는다
    static uint64_t last_log[4] = {0, 0, 0, 0};   // 5초 스로틀 로그 (도배 방지)
    uint64_t now = NowMs();
    if (now - last_log[ch] >= 5000) {
      EmitEvent(ch, "OCR deferred: nothing to read (spaces settled or empty)");
      last_log[ch] = now;
    }
    return;
  }

  char path[64];
  snprintf(path, sizeof(path), "%s/cap_%d.jpg", cfg::kStorageDir, slot);
  auto t0 = std::chrono::steady_clock::now();
  cv::Mat img = cv::imread(path, cv::IMREAD_COLOR);
  double load_ms = std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t0).count();
  if (img.empty()) {
    char m[160];
    snprintf(m, sizeof(m), "OCR ch%d #%d: crop load FAILED (%s)", ch, slot, path);
    EmitEvent(ch, m);
    return;
  }

  // 초소형/납작 크롭(먼 차·부스러기 트랙)은 환각만 만들므로 스킵 — 근거는 config.h 주석.
  if (img.cols < cfg::kMinOcrCropWidth || img.rows < cfg::kMinOcrCropHeight) {
    char m[112];
    snprintf(m, sizeof(m), "OCR skipped ch%d #%d: crop too small (%dx%d < %dx%d)",
             ch, slot, img.cols, img.rows, cfg::kMinOcrCropWidth, cfg::kMinOcrCropHeight);
    EmitEvent(ch, m);
    return;
  }

  // good-shot 품질(선명도) 측정 — 라플라시안 분산. 실측: 정상 100~366, 물렁 10~13.
  cv::Mat gray_q, lap_q;
  cv::cvtColor(img, gray_q, cv::COLOR_BGR2GRAY);
  cv::Laplacian(gray_q, lap_q, CV_64F);
  cv::Scalar lm, ls;
  cv::meanStdDev(lap_q, lm, ls);
  double sharp = ls[0] * ls[0];

  char m0[144];
  snprintf(m0, sizeof(m0), "[good-shot] OCR ch%d #%d crop=%dx%d sharp %.0f (load %.1fms)",
           ch, slot, img.cols, img.rows, sharp, load_ms);
  EmitEvent(ch, m0);

  PlateOcrResult r = plate_ocr_.Recognize(img);
  last_plate_ocr_[ch] = r;

  // 후보 전수 OCR 결과 + 지연시간 (병목 확인용)
  char m1[224];
  char lumbuf[24];
  if (r.crop_lum_fixed > 0)
    snprintf(lumbuf, sizeof(lumbuf), "%.0f>%.0f", r.crop_lum, r.crop_lum_fixed);
  else
    snprintf(lumbuf, sizeof(lumbuf), "%.0f", r.crop_lum);
  snprintf(m1, sizeof(m1), "  tinyLPR(%d cands): \"%s\" (conf %.2f, %.1fms) | win %s | lum %s | color %s | rescue %.1fms",
           r.cand_total, r.tiny_text.c_str(), r.tiny_conf, r.tiny_ms,
           r.win_cand.empty() ? "-" : r.win_cand.c_str(), lumbuf,
           r.color.c_str(), r.denoise_ms);
  EmitEvent(ch, m1);

  // 최종 결과
  char m2[224];
  snprintf(m2, sizeof(m2), "%s  OCR result ch%d #%d -> \"%s\" (conf %.2f, by %s, total %.1fms)%s",
           Hl(cfg::kAnsiInfo), ch, slot, r.text.empty() ? "(none)" : r.text.c_str(),
           r.confidence, r.source.c_str(), r.total_ms + load_ms, Hl(cfg::kAnsiReset));
  EmitEvent(ch, m2);

  // good-shot 결과를 챔피언십에 등록. primary 가중은 선명한 크롭에만 —
  //   물렁한(sharp<50) good-shot 이 선명한 버스트를 가중으로 누르는 사고 방지.
  long oid = plate_store_.last_oid(ch);
  // [헛수고 판정] 판독 결과가 이미 이 채널 칸에 배정된 번호면 — 그 차는 해결됐다.
  //   이 크롭은 재판독 금지로 마킹하고 표도 안 넣는다 (다른 칸이 번호를 기다릴 때
  //   해결된 차의 good-shot 을 무한 재인식하던 사고 봉합, 08-05).
  if (plate_decode::ValidPlateFormat(r.text)) {
    std::string canon_chk = r.text;
    if (plate_db_ready_) {
      std::string t;
      if (plate_db_.Match(canon_chk, &t) == 1) canon_chk = t;
    }
    if (parking_.HasPlate(ch, canon_chk)) {
      futile_slot_[ch] = slot;
      char fm[160];
      snprintf(fm, sizeof(fm), "  crop #%d reads \"%s\" — already assigned, stop rereading",
               slot, canon_chk.c_str());
      EmitEvent(ch, fm);
      return;
    }
  }
  if (oid != 0 && plate_decode::ValidPlateFormat(r.text)) {
    // ★ 즉시확정: good-shot 원본 판독 0.99+ 는 개표 생략. 단 발사 전 충돌 검사 —
    //   투표함에 이미 다른 텍스트가 0.95+ 로 들어와 있으면(=버스트가 반박 중) 보류하고
    //   투표로 넘긴다 (오독 0.99 instant 가 버스트 정답 0.98 을 무시한 22소2542 사건).
    //   (가공 결과는 캡 0.94 라 여기 못 옴 — 원본만 자격.) 즉시확정된 차의 버스트·
    //   재판독·개표는 plate_done_ 으로 전부 차단 (CPU 절약 + 이중판정 방지).
    bool conflict = plate_vote_.HasConflict(ch, oid, r.text, cfg::kInstantConflictConf);
    if (r.confidence >= cfg::kInstantFinalConf && conflict) {
      char cm[128];
      snprintf(cm, sizeof(cm), "  instant hold ch%d id%ld: burst disagrees -> to vote", ch, oid);
      EmitEvent(ch, cm);
    }
    // [구역 합의제] 구역 채널은 단발 즉시확정 금지 — 표로 넣고 그 자리에서 개표 시도.
    //   단발 1.00 오독(49허5578 사건)은 합의(2표)가 막고, 2표가 모이는 순간 확정된다.
    if (parking_.HasZones(ch)) {
      plate_vote_.Add(ch, oid, r.text, r.confidence,
                      /*primary=*/sharp >= cfg::kGoodshotSharpMin);
      TryFinalizeOid(ch, oid, NowMs());  // 즉석 개표 — 추적 목록에 없어도 결론 낸다
    } else if (r.confidence >= cfg::kInstantFinalConf && !conflict) {
      // DB 교정: 즉시확정 텍스트가 미등록인데 유일 1글자 매칭이 있으면 등록 번호로 교정
      std::string fin = r.text;
      const char* dbtag = "";
      if (plate_db_ready_) {
        std::string dbtxt;
        if (plate_db_.Match(fin, &dbtxt) == 1 && dbtxt != fin) {
          fin = dbtxt;
          dbtag = ", db-fix";
        }
      }
      last_final_[ch] = fin;
      RecordFinal(ch, fin, NowMs(), oid);
      plate_done_[ch].insert(oid);
      int dn; double dc; bool dp;
      plate_vote_.Finalize(ch, oid, &dn, &dc, &dp);  // 투표함 비우기(결과 폐기)
      char fm[256];
      snprintf(fm, sizeof(fm), "%s★ PLATE FINAL ch%d id%ld -> \"%s\" (instant, conf %.2f, src=good-shot%s)%s",
               Hl(cfg::kAnsiFinal), ch, oid, fin.c_str(), r.confidence, dbtag,
               Hl(cfg::kAnsiReset));
      EmitEvent(ch, fm);
    } else {
      plate_vote_.Add(ch, oid, r.text, r.confidence,
                      /*primary=*/sharp >= cfg::kGoodshotSharpMin);
    }
  }
}

// 웹페이지(Go App)용 HTTP: /detections?ch=N → 현재 감지 박스 JSON 반환
void SampleComponent::RegisterURI() {
  Vector<String> methods;
  methods.push_back("GET");
  auto* uri = new ("OpenAPI")
      IAppDispatcher::OpenAPIRegistrar(String("/detections"), GetInstanceName(), methods);
  SendNoReplyEvent("AppDispatcher",
                   static_cast<int32_t>(IAppDispatcher::EEventType::eRegisterCommand), 0, uri);

  auto* snap = new ("OpenAPI")
      IAppDispatcher::OpenAPIRegistrar(String("/snapshot"), GetInstanceName(), methods);
  SendNoReplyEvent("AppDispatcher",
                   static_cast<int32_t>(IAppDispatcher::EEventType::eRegisterCommand), 0, snap);

  // /config : 사람/차량 감지 on/off (GET=현재값, POST=변경)
  Vector<String> gp;
  gp.push_back("GET");
  gp.push_back("POST");
  auto* cfg = new ("OpenAPI")
      IAppDispatcher::OpenAPIRegistrar(String("/config"), GetInstanceName(), gp);
  SendNoReplyEvent("AppDispatcher",
                   static_cast<int32_t>(IAppDispatcher::EEventType::eRegisterCommand), 0, cfg);

  // [진단] /rawmeta?ch=N → 그 채널의 마지막 메타데이터 XML 원본 통째로 (번호판 포맷 확인용)
  auto* raw = new ("OpenAPI")
      IAppDispatcher::OpenAPIRegistrar(String("/rawmeta"), GetInstanceName(), methods);
  SendNoReplyEvent("AppDispatcher",
                   static_cast<int32_t>(IAppDispatcher::EEventType::eRegisterCommand), 0, raw);

  // [진단] /rawevents?ch=N → 누적 이벤트 알림 (번호 이벤트 사냥)
  auto* rev = new ("OpenAPI")
      IAppDispatcher::OpenAPIRegistrar(String("/rawevents"), GetInstanceName(), methods);
  SendNoReplyEvent("AppDispatcher",
                   static_cast<int32_t>(IAppDispatcher::EEventType::eRegisterCommand), 0, rev);

  // /lastplate[?id=차량id] → 디스크에 저장된 번호판 best JPEG
  auto* lp = new ("OpenAPI")
      IAppDispatcher::OpenAPIRegistrar(String("/lastplate"), GetInstanceName(), methods);
  SendNoReplyEvent("AppDispatcher",
                   static_cast<int32_t>(IAppDispatcher::EEventType::eRegisterCommand), 0, lp);

  // [진단] /imgref → 카메라 ImageRef 크롭 파일 경로 탐색/읽기 테스트
  auto* iref = new ("OpenAPI")
      IAppDispatcher::OpenAPIRegistrar(String("/imgref"), GetInstanceName(), methods);
  SendNoReplyEvent("AppDispatcher",
                   static_cast<int32_t>(IAppDispatcher::EEventType::eRegisterCommand), 0, iref);

  // [4단계] /eventlog → DiskLogSink 가 쌓은 이벤트 이력 (새 sink 확인용)
  auto* elog = new ("OpenAPI")
      IAppDispatcher::OpenAPIRegistrar(String("/eventlog"), GetInstanceName(), methods);
  SendNoReplyEvent("AppDispatcher",
                   static_cast<int32_t>(IAppDispatcher::EEventType::eRegisterCommand), 0, elog);

  // /platetext?ch=N → 번호판 숫자 인식 결과(tinyLPR+Multi-line 신뢰도 병합) JSON
  auto* ptxt = new ("OpenAPI")
      IAppDispatcher::OpenAPIRegistrar(String("/platetext"), GetInstanceName(), methods);
  SendNoReplyEvent("AppDispatcher",
                   static_cast<int32_t>(IAppDispatcher::EEventType::eRegisterCommand), 0, ptxt);

  // 검증용 갤러리: /platelist (개수), /plate?n=N (누적 저장분)
  auto* plst = new ("OpenAPI")
      IAppDispatcher::OpenAPIRegistrar(String("/platelist"), GetInstanceName(), methods);
  SendNoReplyEvent("AppDispatcher",
                   static_cast<int32_t>(IAppDispatcher::EEventType::eRegisterCommand), 0, plst);
  auto* pl1 = new ("OpenAPI")
      IAppDispatcher::OpenAPIRegistrar(String("/plate"), GetInstanceName(), methods);
  SendNoReplyEvent("AppDispatcher",
                   static_cast<int32_t>(IAppDispatcher::EEventType::eRegisterCommand), 0, pl1);

  // [진단] /sysinfo — 앱이 실제 쓸 수 있는 CPU 코어 수 (스레드 설계용)
  auto* sinfo = new ("OpenAPI")
      IAppDispatcher::OpenAPIRegistrar(String("/sysinfo"), GetInstanceName(), methods);
  SendNoReplyEvent("AppDispatcher",
                   static_cast<int32_t>(IAppDispatcher::EEventType::eRegisterCommand), 0, sinfo);

  // [검증] /candlist (후보 비교 기록 JSON), /cand?n=N (후보 크롭 이미지)
  auto* clst = new ("OpenAPI")
      IAppDispatcher::OpenAPIRegistrar(String("/candlist"), GetInstanceName(), methods);
  SendNoReplyEvent("AppDispatcher",
                   static_cast<int32_t>(IAppDispatcher::EEventType::eRegisterCommand), 0, clst);
  auto* cd1 = new ("OpenAPI")
      IAppDispatcher::OpenAPIRegistrar(String("/cand"), GetInstanceName(), methods);
  SendNoReplyEvent("AppDispatcher",
                   static_cast<int32_t>(IAppDispatcher::EEventType::eRegisterCommand), 0, cd1);

  // [EV 판정] /isev?plate=<URL인코딩 번호> → 등록·전기차·목격 여부 JSON (차량365 연동)
  auto* isev = new ("OpenAPI")
      IAppDispatcher::OpenAPIRegistrar(String("/isev"), GetInstanceName(), methods);
  SendNoReplyEvent("AppDispatcher",
                   static_cast<int32_t>(IAppDispatcher::EEventType::eRegisterCommand), 0, isev);

  // [주차] Pi 연동 — 구역 설정(GET/POST) + 실시간 상태(GET)
  auto* proi = new ("OpenAPI")
      IAppDispatcher::OpenAPIRegistrar(String("/parking_roi"), GetInstanceName(), gp);  // GET+POST
  SendNoReplyEvent("AppDispatcher",
                   static_cast<int32_t>(IAppDispatcher::EEventType::eRegisterCommand), 0, proi);
  auto* pst = new ("OpenAPI")
      IAppDispatcher::OpenAPIRegistrar(String("/parking_status"), GetInstanceName(), methods);  // GET
  SendNoReplyEvent("AppDispatcher",
                   static_cast<int32_t>(IAppDispatcher::EEventType::eRegisterCommand), 0, pst);
  // [튜닝] 주차 판정 런타임 파라미터 (GET=현재값, POST=부분 갱신·즉시 적용·영속화)
  auto* ptune = new ("OpenAPI")
      IAppDispatcher::OpenAPIRegistrar(String("/parking_tune"), GetInstanceName(), gp);  // GET+POST
  SendNoReplyEvent("AppDispatcher",
                   static_cast<int32_t>(IAppDispatcher::EEventType::eRegisterCommand), 0, ptune);

  // [④탐사] /lsdownload?ch=N → /tmp/download/chN 파일 목록
  //   카메라가 oid 당 good-shot 을 여러 장 쓰는지 확인 (여러 장이면 공짜 멀티샘플)
  auto* lsd = new ("OpenAPI")
      IAppDispatcher::OpenAPIRegistrar(String("/lsdownload"), GetInstanceName(), methods);
  SendNoReplyEvent("AppDispatcher",
                   static_cast<int32_t>(IAppDispatcher::EEventType::eRegisterCommand), 0, lsd);
}

bool SampleComponent::HandleHttpRequest(Event* event) {
  if (event->IsReply()) return true;
  auto* oas = reinterpret_cast<OpenAppSerializable*>(event->GetBaseObjectArgument());
  std::string path = oas->GetFCGXParam("PATH_INFO").c_str();

  if (path == "/config") {
    // POST 면 본문 {"person":bool,"vehicle":bool} 로 설정 변경
    if (std::string(oas->GetMethod().c_str()) == "POST") {
      std::string body(oas->GetRequestBody().c_str());
      // "person"/"vehicle" 키 뒤에 true 가 false 보다 먼저 나오면 on
      auto flagAfter = [&](const char* key, bool cur) -> bool {
        size_t k = body.find(key);
        if (k == std::string::npos) return cur;      // 키 없으면 기존값 유지
        size_t t = body.find("true", k);
        size_t f = body.find("false", k);
        if (t == std::string::npos) return false;
        if (f == std::string::npos) return true;
        return t < f;
      };
      detect_person_ = flagAfter("person", detect_person_);
      detect_vehicle_ = flagAfter("vehicle", detect_vehicle_);
      char lg[80];
      snprintf(lg, sizeof(lg), "[config] person=%s vehicle=%s",
               detect_person_ ? "on" : "off", detect_vehicle_ ? "on" : "off");
      EmitEvent(0, lg);
    }
    // GET/POST 모두 현재값 반환
    std::string b = std::string("{\"person\":") + (detect_person_ ? "true" : "false") +
                    ",\"vehicle\":" + (detect_vehicle_ ? "true" : "false") + "}";
    oas->SetResponseBody(b.c_str(), b.size());
  } else if (path == "/parking_status") {
    // [주차] Pi 폴링 — 구역별 실시간 상태 XML
    std::string body = parking_.StatusXml(NowMs());
    oas->AddResponseHeader("Content-type", "application/xml");
    oas->SetResponseBody(body.c_str(), body.size());
  } else if (path == "/parking_tune") {
    // [튜닝] 주차 판정 런타임 파라미터. GET=현재값, POST=부분 갱신(즉시 적용+영속화).
    //   본문에 있는 키만 반영, 범위 밖 값은 그 키만 무시 (park_tune.h 의 범위표).
    if (std::string(oas->GetMethod().c_str()) == "POST") {
      std::string rb(oas->GetRequestBody().c_str());
      if (park_tune_.FromJson(rb)) {
        park_tune_.Save();
        EmitEvent(0, std::string("[tune] applied: ") + park_tune_.ToJson());
      } else {
        oas->SetStatusCode(400);
        oas->SetResponseBody("no valid field — GET /parking_tune for keys");
        return true;
      }
    }
    std::string b = park_tune_.ToJson();
    oas->AddResponseHeader("Content-type", "application/json");
    oas->SetResponseBody(b.c_str(), b.size());
  } else if (path == "/parking_roi") {
    // [주차] 구역 설정. GET=목록, POST=추가(본문에 4점), ?delete=ID 또는 ?clear=1
    std::string qs = oas->GetFCGXParam("QUERY_STRING").c_str();
    int ch = 0;
    size_t cp = qs.find("ch=");
    if (cp != std::string::npos) ch = atoi(qs.c_str() + cp + 3);
    if (ch < 0 || ch >= cfg::kChannels) { oas->SetStatusCode(400); oas->SetResponseBody("bad ch"); return true; }
    std::string method(oas->GetMethod().c_str());
    std::string body;
    if (method == "POST") {
      size_t dp = qs.find("delete=");
      size_t cl = qs.find("clear=");
      if (dp != std::string::npos) {                       // 구역 하나 삭제
        std::string id = qs.substr(dp + 7);
        size_t amp = id.find('&'); if (amp != std::string::npos) id = id.substr(0, amp);
        bool ok = parking_.Remove(ch, id);
        PurgePlateState(ch);  // 구역 변경 = 판독 상태 백지 (이전 세션 plate_done 이월 방지)
        body = std::string("{\"deleted\":") + (ok ? "true" : "false") + "}";
      } else if (cl != std::string::npos) {                // 채널 전체 비움
        parking_.Clear(ch);
        PurgePlateState(ch);
        body = "{\"cleared\":true}";
      } else {                                             // 4점 추가 — 본문에서 숫자 8개 파싱
        std::string rb(oas->GetRequestBody().c_str());
        std::vector<ParkingZone::Pt> pts;
        double nums[8]; int n = 0;
        for (size_t i = 0; i < rb.size() && n < 8;) {
          char c = rb[i];
          if (c == '-' || c == '.' || (c >= '0' && c <= '9')) {
            nums[n++] = atof(rb.c_str() + i);
            while (i < rb.size() && (rb[i] == '-' || rb[i] == '.' ||
                   (rb[i] >= '0' && rb[i] <= '9'))) i++;
          } else i++;
        }
        if (n != 8) { oas->SetStatusCode(400); oas->SetResponseBody("need 4 points (8 numbers)"); return true; }
        for (int k = 0; k < 4; k++) pts.push_back({nums[2 * k], nums[2 * k + 1]});
        // Pi 표시좌표는 반전 저장(기본). HTML 오버레이는 판정좌표계에서 직접 찍으므로 ?noflip=1.
        bool flip = qs.find("noflip=1") == std::string::npos;
        std::string id = parking_.Add(ch, pts, flip);
        if (id.empty()) { oas->SetStatusCode(422); oas->SetResponseBody("add failed"); return true; }
        PurgePlateState(ch);  // 구역 변경 = 판독 상태 백지 (이전 세션 plate_done 이월 방지)
        char lg[80]; snprintf(lg, sizeof(lg), "[parking] zone added: %s", id.c_str());
        EmitEvent(ch, lg);
        body = std::string("{\"id\":\"") + id + "\"}";
      }
      oas->AddResponseHeader("Content-type", "application/json");
      oas->SetResponseBody(body.c_str(), body.size());
    } else {                                               // GET = 목록
      body = parking_.RoiXml(ch);
      oas->AddResponseHeader("Content-type", "application/xml");
      oas->SetResponseBody(body.c_str(), body.size());
    }
  } else if (path == "/detections") {
    int ch = 0;
    std::string qs = oas->GetFCGXParam("QUERY_STRING").c_str();
    size_t p = qs.find("ch=");
    if (p != std::string::npos) ch = atoi(qs.c_str() + p + 3);
    if (ch < 0 || ch >= cfg::kChannels) ch = 0;

    std::string body = "{\"ch\":" + std::to_string(ch) + ",\"objects\":[";
    bool first = true;
    for (auto& d : latest_[ch]) {
      char buf[192];
      snprintf(buf, sizeof(buf),
               "%s{\"id\":%ld,\"moving\":%s,\"plate\":%s,\"box\":[%.4f,%.4f,%.4f,%.4f]}",
               first ? "" : ",", d.id, d.moving ? "true" : "false",
               d.plate ? "true" : "false", d.l, d.t, d.r, d.b);
      body += buf;
      first = false;
    }
    body += "]}";
    oas->SetResponseBody(body.c_str(), body.size());
  } else if (path == "/rawmeta") {
    // [진단] 마지막 메타데이터 XML 원본 그대로 반환 (번호판 실제 포맷 확인용)
    int ch = 0;
    std::string qs = oas->GetFCGXParam("QUERY_STRING").c_str();
    size_t p = qs.find("ch=");
    if (p != std::string::npos) ch = atoi(qs.c_str() + p + 3);
    if (ch < 0 || ch >= cfg::kChannels) ch = 0;
    // 번호판 든 프레임이 있으면 그걸, 없으면 차량 프레임을 반환
    const std::string& out = !last_plate_xml_[ch].empty() ? last_plate_xml_[ch] : last_xml_[ch];
    oas->SetResponseBody(out.c_str(), out.size());
  } else if (path == "/rawevents") {
    // [진단] 누적된 이벤트 알림들 (번호 이벤트가 오는지 확인용)
    int ch = 0;
    std::string qs = oas->GetFCGXParam("QUERY_STRING").c_str();
    size_t p = qs.find("ch=");
    if (p != std::string::npos) ch = atoi(qs.c_str() + p + 3);
    if (ch < 0 || ch >= cfg::kChannels) ch = 0;
    oas->SetResponseBody(raw_events_[ch].c_str(), raw_events_[ch].size());
  } else if (path == "/lastplate") {
    // 디스크에 저장된 번호판 best JPEG (id 주면 그 차량, 없으면 최신)
    std::string qs = oas->GetFCGXParam("QUERY_STRING").c_str();
    char fpath[96];
    size_t ip = qs.find("id=");
    if (ip != std::string::npos)
      snprintf(fpath, sizeof(fpath), "%s/plate_%ld.jpg", cfg::kStorageDir, atol(qs.c_str() + ip + 3));
    else
      snprintf(fpath, sizeof(fpath), "%s/plate_last.jpg", cfg::kStorageDir);
    std::ifstream ifs(fpath, std::ios::binary);
    if (ifs) {
      std::ostringstream oss; oss << ifs.rdbuf();
      std::string data = oss.str();
      if (!data.empty()) {
        oas->SetStatusCode(200);
        oas->SetResponseBody(data, OpenAppResponseType::FILE);
      } else { oas->SetStatusCode(404); oas->SetResponseBody("empty"); }
    } else { oas->SetStatusCode(404); oas->SetResponseBody("no saved plate"); }
  } else if (path == "/platelist") {
    // [검증] 저장 개수 + 진단 카운터 (번호판 본 수 / ImageRef 온 수 / 읽기실패 수)
    auto arr4 = [](const int* a) {
      return "[" + std::to_string(a[0]) + "," + std::to_string(a[1]) + "," +
             std::to_string(a[2]) + "," + std::to_string(a[3]) + "]";
    };
    std::string body = "{\"total\":" + std::to_string(plate_store_.total()) +
                       ",\"ring\":" + std::to_string(cfg::kRingSize) +
                       ",\"plates_seen\":" + std::to_string(plates_seen_) +
                       ",\"imgref_seen\":" + std::to_string(imgref_seen_) +
                       ",\"read_fail\":" + std::to_string(plate_store_.read_fail()) +
                       ",\"imgref_ch\":" + arr4(imgref_ch_) +
                       ",\"saved_ch\":" + arr4(plate_store_.saved_ch()) + "}";
    oas->AddResponseHeader("Content-type", "application/json");
    oas->SetResponseBody(body.c_str(), body.size());
  } else if (path == "/plate") {
    // [검증] 누적 저장분 n번째 슬롯 이미지: ../storage/cap_<n>.jpg
    int n = 0;
    std::string qs = oas->GetFCGXParam("QUERY_STRING").c_str();
    size_t p = qs.find("n="); if (p != std::string::npos) n = atoi(qs.c_str() + p + 2);
    char fp[64]; snprintf(fp, sizeof(fp), "%s/cap_%d.jpg", cfg::kStorageDir, n);
    std::ifstream ifs(fp, std::ios::binary);
    if (ifs) {
      std::ostringstream oss; oss << ifs.rdbuf();
      std::string d = oss.str();
      if (!d.empty()) { oas->SetStatusCode(200); oas->SetResponseBody(d, OpenAppResponseType::FILE); }
      else { oas->SetStatusCode(404); oas->SetResponseBody("empty"); }
    } else { oas->SetStatusCode(404); oas->SetResponseBody("no cap"); }
  } else if (path == "/imgref") {
    // [진단] 카메라가 준 번호판 크롭(ImageRef) 파일을 여러 후보 경로로 읽어보고, 읽히면 저장
    int ch = 0;
    std::string qs = oas->GetFCGXParam("QUERY_STRING").c_str();
    size_t p = qs.find("ch="); if (p != std::string::npos) ch = atoi(qs.c_str() + p + 3);
    if (ch < 0 || ch >= cfg::kChannels) ch = 0;
    std::string ref = last_imgref_[ch];
    const char* bases[] = {"", "/tmp", "..", "../..", "../../..", "/mnt/data", "/opt"};
    std::string body = "{\"imgref\":\"" + ref + "\",\"tries\":[";
    std::string found;
    for (int i = 0; i < 7; i++) {
      std::string fp = std::string(bases[i]) + ref;
      long sz = -1; bool jpg = false;
      std::ifstream ifs(fp.c_str(), std::ios::binary);
      if (ifs) {
        std::ostringstream oss; oss << ifs.rdbuf();
        std::string d = oss.str(); sz = (long)d.size();
        size_t n = d.size();
        jpg = (n > 4 && (unsigned char)d[0] == 0xFF && (unsigned char)d[1] == 0xD8);
        if (jpg && found.empty()) {  // 첫 유효 JPEG → plate_last.jpg 로 저장
          found = fp;
          std::string lp = std::string(cfg::kStorageDir) + "/plate_last.jpg";
          std::ofstream ofs(lp.c_str(), std::ios::binary);
          if (ofs) ofs.write(d.data(), d.size());
        }
      }
      char buf[256];
      snprintf(buf, sizeof(buf), "%s{\"path\":\"%s\",\"size\":%ld,\"jpg\":%s}",
               i ? "," : "", fp.c_str(), sz, jpg ? "true" : "false");
      body += buf;
    }
    body += "],\"found\":\"" + found + "\"}";
    oas->AddResponseHeader("Content-type", "application/json");
    oas->SetResponseBody(body.c_str(), body.size());
  } else if (path == "/eventlog") {
    // [4단계] DiskLogSink 가 파일에 쌓은 이벤트 이력 그대로 반환 (새 sink 동작 확인)
    std::string lp = std::string(cfg::kStorageDir) + "/events.log";
    std::ifstream ifs(lp.c_str(), std::ios::binary);
    if (ifs) {
      std::ostringstream oss; oss << ifs.rdbuf();
      std::string d = oss.str();
      oas->SetResponseBody(d.c_str(), d.size());
    } else {
      oas->SetResponseBody("(no events yet)");
    }
  } else if (path == "/platetext") {
    // 채널별 마지막 번호판 숫자 인식 결과 (tinyLPR+Multi-line 신뢰도 병합)
    int ch = 0;
    std::string qs = oas->GetFCGXParam("QUERY_STRING").c_str();
    size_t p = qs.find("ch="); if (p != std::string::npos) ch = atoi(qs.c_str() + p + 3);
    if (ch < 0 || ch >= cfg::kChannels) ch = 0;
    const PlateOcrResult& r = last_plate_ocr_[ch];
    char body[512];
    snprintf(body, sizeof(body),
             "{\"ocr_ready\":%s,\"ch\":%d,\"text\":\"%s\",\"confidence\":%.3f,\"source\":\"%s\","
             "\"final\":\"%s\","
             "\"tiny\":{\"text\":\"%s\",\"conf\":%.3f,\"ms\":%.1f},"
             "\"total_ms\":%.1f}",
             plate_ocr_ready_ ? "true" : "false", ch, r.text.c_str(), r.confidence, r.source.c_str(),
             last_final_[ch].c_str(),
             r.tiny_text.c_str(), r.tiny_conf, r.tiny_ms, r.total_ms);
    oas->AddResponseHeader("Content-type", "application/json");
    oas->SetResponseBody(body, strlen(body));
  } else if (path == "/isev") {
    // [EV 판정] 차량365 연동: 번호로 등록·전기차·현장 목격 여부 응답.
    //   ev    = 등록 DB 의 ",ev" 플래그 (등록 시점 ev.or.kr 조회로 확정 — 권위 있는 답)
    //   color = 최근 FINAL 목격 시 판 색(wh/ye/gr/bl) — 참고용 원시값. EV 판정엔 쓰지
    //           않는다: 법인 전기차는 연두색이라 "파랑=EV" 교차확인이 틀린다 (07-30).
    std::string qs = oas->GetFCGXParam("QUERY_STRING").c_str();
    std::string plate;
    size_t p = qs.find("plate=");
    if (p != std::string::npos) {
      size_t e = qs.find('&', p);
      plate = UrlDecode(qs.substr(p + 6, e == std::string::npos ? std::string::npos : e - (p + 6)));
    }
    bool registered = false, ev = false;
    std::string canon = plate;                 // 매칭된 등록 번호 (ed<=1 교정 포함)
    if (!plate.empty() && plate_db_ready_) {
      std::string dbtxt;
      if (plate_db_.Match(plate, &dbtxt) == 1) {
        registered = true;
        canon = dbtxt;
        ev = plate_db_.IsEv(dbtxt);
      }
    }
    bool seen = false;
    std::string seen_color = "?";
    long age_ms = -1;
    uint64_t now = NowMs();
    for (auto it = finals_log_.rbegin(); it != finals_log_.rend(); ++it) {  // 최신부터
      if (it->text == canon) {
        seen = true;
        seen_color = it->color;   // 원시 판 색 (참고용) — EV 판정엔 미사용
        age_ms = (long)(now - it->ms);
        break;
      }
    }
    char body[320];
    snprintf(body, sizeof(body),
             "{\"plate\":\"%s\",\"registered\":%s,\"ev\":%s,"
             "\"seen\":%s,\"color\":\"%s\",\"age_ms\":%ld}",
             canon.c_str(), registered ? "true" : "false", ev ? "true" : "false",
             seen ? "true" : "false", seen_color.c_str(), age_ms);
    oas->AddResponseHeader("Content-type", "application/json");
    oas->SetResponseBody(body, strlen(body));
  } else if (path == "/lsdownload") {
    // [④탐사] /tmp/download/chN 파일 목록 — 카메라가 oid 당 크롭을 몇 장 쓰는지 확인
    int ch = 0;
    std::string qs = oas->GetFCGXParam("QUERY_STRING").c_str();
    size_t p = qs.find("ch="); if (p != std::string::npos) ch = atoi(qs.c_str() + p + 3);
    if (ch < 0 || ch >= cfg::kChannels) ch = 0;
    char dirp[64];
    snprintf(dirp, sizeof(dirp), "%s/download/ch%d", cfg::kTmpPrefix, ch);
    std::string body = std::string("{\"dir\":\"") + dirp + "\",\"files\":[";
    int cnt = 0;
    DIR* d = opendir(dirp);
    if (d) {
      struct dirent* de;
      while ((de = readdir(d)) != nullptr) {
        if (de->d_name[0] == '.') continue;
        char fpath[384];   // dirp(64) + '/' + d_name(255) — -Werror=format-truncation 여유
        snprintf(fpath, sizeof(fpath), "%s/%s", dirp, de->d_name);
        struct stat st{};
        long sz = (stat(fpath, &st) == 0) ? (long)st.st_size : -1;
        long mt = (long)st.st_mtime;
        char row[384];
        snprintf(row, sizeof(row), "%s{\"name\":\"%s\",\"size\":%ld,\"mtime\":%ld}",
                 cnt ? "," : "", de->d_name, sz, mt);
        body += row;
        cnt++;
      }
      closedir(d);
    }
    char tail[48];
    snprintf(tail, sizeof(tail), "],\"count\":%d}", cnt);
    body += tail;
    oas->AddResponseHeader("Content-type", "application/json");
    oas->SetResponseBody(body.c_str(), body.size());
  } else if (path == "/sysinfo") {
    // [진단] 앱이 실제 쓸 수 있는 코어 수 확인 (스레드 설계용)
    unsigned hc = std::thread::hardware_concurrency();  // 런타임 가용 코어(보통 온라인 CPU 수)
    long online = sysconf(_SC_NPROCESSORS_ONLN);        // 온라인 CPU
    long conf = sysconf(_SC_NPROCESSORS_CONF);          // 구성된 전체 CPU
    // /proc/cpuinfo 의 "processor" 라인 수 세기
    int proc_cnt = 0;
    std::ifstream ci("/proc/cpuinfo");
    if (ci) {
      std::string line;
      while (std::getline(ci, line))
        if (line.compare(0, 9, "processor") == 0) proc_cnt++;
    }
    char body[256];
    snprintf(body, sizeof(body),
             "{\"hardware_concurrency\":%u,\"online\":%ld,\"configured\":%ld,"
             "\"cpuinfo_processors\":%d}",
             hc, online, conf, proc_cnt);
    oas->AddResponseHeader("Content-type", "application/json");
    oas->SetResponseBody(body, strlen(body));
  } else if (path == "/candlist") {
    // [검증] 최근 후보 비교 기록(JSON). decision: N=new save, R=replace(win), K=keep(dropped)
    std::string body = "[";
    bool first = true;
    for (const auto& c : plate_store_.cand_log()) {
      char buf[160];
      snprintf(buf, sizeof(buf),
               "%s{\"seq\":%ld,\"file\":%d,\"slot\":%d,\"ch\":%d,\"oid\":%ld,\"score\":%.0f,\"d\":\"%c\"}",
               first ? "" : ",", c.seq, c.file, c.slot, c.ch, c.oid, c.score, c.decision);
      body += buf;
      first = false;
    }
    body += "]";
    oas->AddResponseHeader("Content-type", "application/json");
    oas->SetResponseBody(body.c_str(), body.size());
  } else if (path == "/cand") {
    // [검증] 후보 크롭 이미지 n번: ../storage/cand_<n>.jpg
    int n = 0;
    std::string qs = oas->GetFCGXParam("QUERY_STRING").c_str();
    size_t p = qs.find("n="); if (p != std::string::npos) n = atoi(qs.c_str() + p + 2);
    char fp[64]; snprintf(fp, sizeof(fp), "%s/cand_%d.jpg", cfg::kStorageDir, n);
    std::ifstream ifs(fp, std::ios::binary);
    if (ifs) {
      std::ostringstream oss; oss << ifs.rdbuf();
      std::string d = oss.str();
      if (!d.empty()) { oas->SetStatusCode(200); oas->SetResponseBody(d, OpenAppResponseType::FILE); }
      else { oas->SetStatusCode(404); oas->SetResponseBody("empty"); }
    } else { oas->SetStatusCode(404); oas->SetResponseBody("no cand"); }
  } else if (path == "/snapshot") {
    // 라이브 뷰: 스냅샷 새로고침 + 캐시된 완성 프레임 즉시 응답
    int ch = 0;
    std::string qs = oas->GetFCGXParam("QUERY_STRING").c_str();
    size_t p = qs.find("ch=");
    if (p != std::string::npos) ch = atoi(qs.c_str() + p + 3);
    if (ch < 0 || ch >= cfg::kChannels) ch = 0;

    RefreshSnapshot(ch);
    std::string out;
    { std::lock_guard<std::mutex> lk(jpeg_mtx_); out = last_jpeg_[ch]; }
    if (!out.empty()) {
      oas->SetStatusCode(200);
      oas->SetResponseBody(out, OpenAppResponseType::FILE);
    } else {
      oas->SetStatusCode(503);
      oas->SetResponseBody("warming up");
    }
  }
  return true;
}

// 카메라에 스냅샷 1장 주문(과부하 방지 100ms 간격) + 완성 JPEG 파일이면 last_jpeg_ 갱신.
//  스트리밍(/snapshot)이든 버스트든 이걸 불러 쓴다 → 스트리밍 꺼도 캡처 가능.
void SampleComponent::RefreshSnapshot(int ch) {
  if (ch < 0 || ch >= cfg::kChannels) return;
  char jpath[64];
  snprintf(jpath, sizeof(jpath), "%s/snap%d.jpg", cfg::kStorageDir, ch);

  uint64_t now = NowMs();
  if (now - last_snap_trigger_[ch] >= 100) {
    last_snap_trigger_[ch] = now;
    JsonUtility::JsonDocument doc(JsonUtility::Type::kObjectType);
    auto& alloc = doc.GetAllocator();
    doc.AddMember("jpeg_path", rapidjson::Value(jpath, alloc), alloc);
    doc.AddMember("channel", rapidjson::Value(std::to_string(ch).c_str(), alloc), alloc);
    doc.AddMember("app_name", rapidjson::Value(app_id_.c_str(), alloc), alloc);
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    doc.Accept(w);
    SendNoReplyEvent(
        (uint64_t)Component::EReceivers::eOpenPlatformManager,
        (int32_t)IPOpenPlatformManager::EAppEventType::eAppSnapshotJpeg, 0,
        new ("Query") Platform_Std_Refine::SerializableString(sb.GetString()));
  }

  std::ifstream ifs(jpath, std::ios::binary);
  if (ifs) {
    std::ostringstream oss;
    oss << ifs.rdbuf();
    std::string data = oss.str();
    size_t n = data.size();
    if (n > 4 &&
        (unsigned char)data[0] == 0xFF && (unsigned char)data[1] == 0xD8 &&
        (unsigned char)data[n - 2] == 0xFF && (unsigned char)data[n - 1] == 0xD9) {
      std::lock_guard<std::mutex> lk(jpeg_mtx_);
      last_jpeg_[ch].swap(data);
      jpeg_ver_[ch]++;   // [신선도] 새 프레임 표시 — 버스트가 같은 프레임 재탕 못 하게
    }
  }
}

// (번호판 저장 로직 SaveCameraPlate/TrySavePlateFile/ParseObjectId 는 [3단계]에서
//  core/plate_store.h 의 PlateStore 로 이동)

// [시간축 버스트] 추적 중인 번호판(bbox)을 스냅샷에서 직접 잘라 OCR 샘플로 누적.
//   - 스냅샷 지연으로 bbox 가 어긋날 수 있어 여백을 넉넉히(가로35%/세로60%) 주고,
//     그 부정확함은 PlateOcr 의 멀티후보 트리밍이 흡수한다.
//   - oid 당 250ms 스로틀 + 최대 8샘플 (PlateVote 가 관리).
void SampleComponent::BurstSample(int ch, long oid, float l, float t, float r, float b,
                                  uint64_t now_ms, bool zone) {
  if (!plate_ocr_ready_ || r <= l || b <= t) return;
  // [판독 게이트 — 주차 전용 모드] 읽을 일(zone=NeedsRead)이 있을 때만 샘플 (B안).
  if (!zone) return;
  if (plate_done_[ch].count(oid)) return;  // 즉시확정된 차 — 표도 CPU 도 불필요

  // [신선도] 같은 스냅샷 프레임 재판독 금지 — 스냅샷 갱신이 판독보다 느리면 낡은
  //   프레임(이전 차!)을 연사로 재탕해 유령 표를 만든다 ("10버5781" 12연발 실측:
  //   conf·박스·텍스트 완전 동일 = 같은 이미지 12회 판독). 새 프레임만 표로 인정.
  std::string jpg;
  uint64_t ver;
  {
    std::lock_guard<std::mutex> lk(jpeg_mtx_);
    ver = jpeg_ver_[ch];
    jpg = last_jpeg_[ch];
  }
  RefreshSnapshot(ch);  // 다음 프레임 주문 (스로틀 내장)
  if (jpg.empty() || ver == 0) return;
  if (burst_ver_[ch][oid] == ver) return;   // 아직 같은 프레임 — 새것 오면 판독
  if (!plate_vote_.CanSample(ch, oid, now_ms, zone)) return;  // zone=연사 모드
  burst_ver_[ch][oid] = ver;
  if (burst_ver_[ch].size() > 512) burst_ver_[ch].clear();  // 폭주 방지

  std::vector<uchar> buf(jpg.begin(), jpg.end());
  cv::Mat frame = cv::imdecode(buf, cv::IMREAD_COLOR);
  if (frame.empty()) return;

  // 좌표 정규화(0~1): 파서가 픽셀좌표를 주는 경우 프레임 크기로 나눔
  double fw = frame_w_[ch] > 1.0 ? frame_w_[ch] : 1.0;
  double fh = frame_h_[ch] > 1.0 ? frame_h_[ch] : 1.0;
  double L = l, T = t, R = r, B = b;
  if (R > 1.5 || B > 1.5) { L /= fw; R /= fw; T /= fh; B /= fh; }

  // 지연 보정 여백
  double bw = R - L, bh = B - T;
  L -= bw * 0.35; R += bw * 0.35;
  T -= bh * 0.60; B += bh * 0.60;
  int x0 = std::max(0, (int)(L * frame.cols)), x1 = std::min(frame.cols, (int)(R * frame.cols));
  int y0 = std::max(0, (int)(T * frame.rows)), y1 = std::min(frame.rows, (int)(B * frame.rows));
  if (x1 - x0 < 48 || y1 - y0 < 16) return;  // 너무 작으면(멀면) 스킵

  cv::Mat crop = frame(cv::Rect(x0, y0, x1 - x0, y1 - y0));
  // 버스트는 경량 파이프라인(후보 3, 구조대 생략) — 참고 표에 풀코스는 과함(부하 다이어트)
  PlateOcrResult br = plate_ocr_.Recognize(crop, /*light=*/true);
  // 스냅샷-bbox 시간 어긋남 시 tinyLPR 이 conf 0.5~0.85 짜리 "환각 번호판"을 지어냄(실측).
  // 진짜 판독은 0.93+ 로 관측 → 0.90 미만은 전부 버린다.
  if (br.text.empty() || br.confidence < 0.90) return;
  plate_vote_.Add(ch, oid, br.text, br.confidence, /*primary=*/false);

  // [스태킹] 유효 샘플(=판을 제대로 프레이밍한 크롭)만 고정 캔버스에 합산 누적.
  //   크기 정규화는 스트레치 리사이즈 — 마진이 bbox 비례라 프레임 간 정렬이 유지된다.
  if (cfg::kBurstStack) {
    static const int SW = 512, SH = 208;
    cv::Mat g;
    cv::cvtColor(crop, g, cv::COLOR_BGR2GRAY);
    cv::Mat rs;
    cv::resize(g, rs, cv::Size(SW, SH), 0, 0, cv::INTER_AREA);
    StackAcc& a = stack_acc_[ch][oid];
    if (a.sum.empty()) a.sum = cv::Mat::zeros(SH, SW, CV_32FC1);
    cv::Mat f;
    rs.convertTo(f, CV_32F);
    a.sum += f;
    a.n++;
  }
  char m[224];
  snprintf(m, sizeof(m), "%s  burst ch%d id%ld sample#%d \"%s\" (conf %.2f, box %dx%d, %.1fms)%s",
           Hl(cfg::kAnsiDim), ch, oid, plate_vote_.Count(ch, oid), br.text.c_str(), br.confidence,
           x1 - x0, y1 - y0, br.total_ms, Hl(cfg::kAnsiReset));
  EmitEvent(ch, m);
}

// [최후 판독] 카메라가 번호판 객체를 안 주는 정지차 — 구역 영역을 스냅샷에서 직접
//   잘라 OCR (08-04 실측: PARKED 후 plate detected 0건 → 크롭 경로 전무 → ⏳ 영구).
//   구역 bbox 하단 중앙(판 예상 위치)과 구역 전체, 창 2개로 시도. 표는 점유차 oid 로
//   적립 → 즉석 개표 → AssignAt 이 구역 중심좌표로 그 칸에 명중.
void SampleComponent::ZoneFallbackOcr(int ch, uint64_t now_ms) {
  if (!plate_ocr_ready_) return;
  auto rects = parking_.PlatelessOccupiedRects(ch);
  if (rects.empty()) return;

  std::string jpg;
  uint64_t ver;
  {
    std::lock_guard<std::mutex> lk(jpeg_mtx_);
    ver = jpeg_ver_[ch];
    jpg = last_jpeg_[ch];
  }
  RefreshSnapshot(ch);  // 다음 프레임 주문
  if (jpg.empty() || ver == 0 || ver == zfb_ver_[ch]) return;  // 새 프레임에서만
  zfb_ver_[ch] = ver;

  std::vector<uchar> buf(jpg.begin(), jpg.end());
  cv::Mat frame = cv::imdecode(buf, cv::IMREAD_COLOR);
  if (frame.empty()) return;

  for (const auto& pr : rects) {
    const double W = pr.r - pr.l, H = pr.b - pr.t;
    // [수색 창] 구역 통짜 크롭은 판을 찌그러뜨려 쓰레기만 나온다 (466x385 → "45어5775"
    //   실측). 번호판 비율(가로로 긴) 창으로 하단을 훑는다: 폭 55%·높이 30% 창을
    //   가로 3지점 × 세로 2줄 = 6개 + 하단 절반 통짜(백업) 1개. 첫 판독 성공 시 중단.
    std::vector<std::array<double, 4>> wins;
    const double ww = W * 0.70, wh = H * 0.32;  // 판이 창에 통째로 들어오게 넉넉히
    // 세로 3줄 — 구역을 차 발밑에 작게 그리면 판이 구역 중간~위에 걸린다 (08-05 와꾸)
    for (double cyf : {0.38, 0.60, 0.82})
      for (double cxf : {0.30, 0.50, 0.70})
        wins.push_back({pr.l + W * cxf - ww / 2, pr.t + H * cyf - wh / 2,
                        pr.l + W * cxf + ww / 2, pr.t + H * cyf + wh / 2});
    wins.push_back({pr.l, pr.t, pr.r, pr.b});  // 백업: 구역 전체
    bool got = false;
    std::string best_txt;                 // [진단] 기준 미달 중 최고 시도 (실패 원인 가시화)
    double best_conf = 0.0;
    int best_win = -1, best_w = 0, best_h = 0, win_i = -1;
    for (const auto& w : wins) {
      ++win_i;
      int x0 = std::max(0, (int)(w[0] * frame.cols));
      int y0 = std::max(0, (int)(w[1] * frame.rows));
      int x1 = std::min(frame.cols, (int)(w[2] * frame.cols));
      int y1 = std::min(frame.rows, (int)(w[3] * frame.rows));
      // 높이 48px 미만 크롭은 모델 입력(96px) 대비 2배+ 뻥튀기 — 쓰레기 표만 만든다
      //   (08-06 실측: box 265x29 → "19도3331" 류 환각 도배)
      if (x1 - x0 < 96 || y1 - y0 < 48) continue;
      cv::Mat crop = frame(cv::Rect(x0, y0, x1 - x0, y1 - y0));
      PlateOcrResult zr = plate_ocr_.Recognize(crop, /*light=*/true);
      if (!zr.text.empty() && zr.confidence > best_conf) {
        best_conf = zr.confidence; best_txt = zr.text;
        best_win = win_i; best_w = x1 - x0; best_h = y1 - y0;
      }
      if (zr.text.empty() || zr.confidence < 0.90) continue;  // 환각 컷 (버스트와 동일)
      // 표 적립 (점유차 oid) + 배정 좌표를 구역 중심으로 고정 + 즉석 개표
      plate_pos_[ch][pr.oid] = {(pr.l + pr.r) / 2.0, (pr.t + pr.b) / 2.0};
      plate_vote_.Add(ch, pr.oid, zr.text, zr.confidence, /*primary=*/false);
      char m[224];
      snprintf(m, sizeof(m), "%s  zone-ocr ch%d %s \"%s\" (conf %.2f, box %dx%d, %.1fms)%s",
               Hl(cfg::kAnsiDim), ch, pr.id.c_str(), zr.text.c_str(), zr.confidence,
               x1 - x0, y1 - y0, zr.total_ms, Hl(cfg::kAnsiReset));
      EmitEvent(ch, m);
      got = true;
      TryFinalizeOid(ch, pr.oid, now_ms);
      break;  // 이 칸은 이번 프레임 몫 완료 — 다음 프레임에서 새 표 적립
    }
    if (!got) {  // 수색 실패 — 최고 시도까지 공개 (원인: 창 밖인지, 흐림인지, 환각인지)
      static uint64_t diag_ms[4] = {0, 0, 0, 0};
      if (now_ms - diag_ms[ch] >= 6000) {
        diag_ms[ch] = now_ms;
        char m[224];
        if (best_win >= 0)
          snprintf(m, sizeof(m),
                   "  zone-ocr ch%d %s: no read in %d wins — best try \"%s\" conf %.2f @win%d %dx%d",
                   ch, pr.id.c_str(), (int)wins.size(), best_txt.c_str(), best_conf,
                   best_win, best_w, best_h);
        else
          snprintf(m, sizeof(m),
                   "  zone-ocr ch%d %s: no read in %d wins — all windows returned nothing",
                   ch, pr.id.c_str(), (int)wins.size());
        EmitEvent(ch, m);
      }
    }
  }
}

// [같은 판 판정] 같은 글자수 + 위치 불일치 ≤2 — 저화질 오독(혼동쌍 1~2글자)은 같은
//   판으로 인정. 빈 배경 환각은 체크마다 전혀 다른 번호가 나와 여기서 걸러진다.
static bool SimilarPlate(const std::string& a, const std::string& b) {
  if (a.empty() || b.empty()) return false;
  auto dec = [](const std::string& s) {          // UTF-8 → 코드포인트 (한글 1글자=1요소)
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
      unsigned char c = s[i];
      uint32_t cp = c;
      int n = 1;
      if (c >= 0xF0) { cp = c & 0x07; n = 4; }
      else if (c >= 0xE0) { cp = c & 0x0F; n = 3; }
      else if (c >= 0xC0) { cp = c & 0x1F; n = 2; }
      for (int k = 1; k < n && i + k < s.size(); k++) cp = (cp << 6) | (s[i + k] & 0x3F);
      out.push_back(cp);
      i += n;
    }
    return out;
  };
  std::vector<uint32_t> x = dec(a), y = dec(b);
  if (x.size() != y.size()) return false;
  int diff = 0;
  for (size_t i = 0; i < x.size(); i++)
    if (x[i] != y[i] && ++diff > 2) return false;
  return true;
}

// [출차 육안검증] 부재 의심 칸(추적 증거가 유예 넘게 끊김)을 스냅샷으로 직접 확인.
//   빈칸이 3연속 확인될 때만 출차 확정. 부재만으로 출차 내리던 옛 폴백의 대체 —
//   WiseAI 는 정지차를 몇 초 만에 메타데이터에서 빼버려 부재≠출차 (08-06 가짜 출차 수리).
//   [환각 방역] 빈 배경(책 표지 등)도 conf 0.6~0.8 헛번호를 뱉는다 (08-06 실측:
//   "70가7071"→"27구7553"→"58머5775" 체크마다 다른 번호 = 환각의 지문 → 빈 칸이 영영
//   안 비워짐). 그래서 "번호가 읽혔다"는 증거가 아니고, "그 칸의 확정 번호(없으면 직전
//   체크의 읽기)와 같은 판으로 보이는 번호가 읽혔다"만 존재 증거로 인정한다.
void SampleComponent::PresenceCheck(int ch, const std::vector<ParkingZone::PendingRect>& rects,
                                    uint64_t now_ms) {
  // [튜닝: 검증 끔] presence_miss=0 — WiseAI 정지차 감지가 안정적인 셋업(최소 객체
  //   12px)에선 부재 유예 자체가 출차 증거. 스냅샷 확인 없이 즉시 출차 (~3초).
  if (park_tune_.presence_miss == 0) {
    for (const auto& pr : rects)
      if (parking_.ForceLeave(ch, pr.id, now_ms)) {
        presence_txt_[ch].erase(pr.id);
        EmitEvent(ch, "🅿️ LEFT " + pr.id + " — 부재 지속, 즉시 출차 (육안검증 off)");
        PurgePlateState(ch);
        CheckParkingEvent(ch);
      }
    return;
  }
  if (!plate_ocr_ready_) return;
  std::string jpg;
  uint64_t ver;
  {
    std::lock_guard<std::mutex> lk(jpeg_mtx_);
    ver = jpeg_ver_[ch];
    jpg = last_jpeg_[ch];
  }
  RefreshSnapshot(ch);  // 다음 프레임 주문
  if (jpg.empty() || ver == 0 || ver == presence_ver_[ch]) return;  // 새 프레임에서만
  presence_ver_[ch] = ver;

  std::vector<uchar> buf(jpg.begin(), jpg.end());
  cv::Mat frame = cv::imdecode(buf, cv::IMREAD_COLOR);
  if (frame.empty()) return;

  for (const auto& pr : rects) {
    // 수색 창은 zone-ocr 와 동일 배치 (판이 있을 만한 자리 전부)
    const double W = pr.r - pr.l, H = pr.b - pr.t;
    std::vector<std::array<double, 4>> wins;
    const double ww = W * 0.70, wh = H * 0.32;
    // [정조준] 번호가 마지막으로 읽힌 좌표를 1순위 창으로 — 구역을 작게 그려 판이
    //   구역 가장자리/밖에 걸려도 검증이 판을 본다 (08-06 실측: plate pos in_zone=0
    //   상태에서 구역 안 창들만 뒤져 쓰레기 읽고 가짜 LEFT).
    if (pr.px > 0.0 || pr.py > 0.0)
      wins.push_back({pr.px - ww / 2, pr.py - wh / 2, pr.px + ww / 2, pr.py + wh / 2});
    for (double cyf : {0.38, 0.60, 0.82})
      for (double cxf : {0.30, 0.50, 0.70})
        wins.push_back({pr.l + W * cxf - ww / 2, pr.t + H * cyf - wh / 2,
                        pr.l + W * cxf + ww / 2, pr.t + H * cyf + wh / 2});
    wins.push_back({pr.l, pr.t, pr.r, pr.b});
    std::string btxt;
    double best = 0.0;
    for (const auto& w : wins) {
      int x0 = std::max(0, (int)(w[0] * frame.cols));
      int y0 = std::max(0, (int)(w[1] * frame.rows));
      int x1 = std::min(frame.cols, (int)(w[2] * frame.cols));
      int y1 = std::min(frame.rows, (int)(w[3] * frame.rows));
      if (x1 - x0 < 96 || y1 - y0 < 48) continue;
      cv::Mat crop = frame(cv::Rect(x0, y0, x1 - x0, y1 - y0));
      PlateOcrResult zr = plate_ocr_.Recognize(crop, /*light=*/true);
      if (!zr.text.empty() && zr.confidence > best) { best = zr.confidence; btxt = zr.text; }
      if (best >= park_tune_.presence_conf && !pr.plate.empty() && SimilarPlate(btxt, pr.plate))
        break;  // 확정 번호와 같은 판 확인 — 더 볼 필요 없음
    }
    // 존재 판정: 읽힌 번호가 "그 칸의 판"으로 보일 때만 (환각은 매번 다른 번호 → 탈락)
    bool present = false;
    if (best >= park_tune_.presence_conf) {
      if (!pr.plate.empty()) {
        present = SimilarPlate(btxt, pr.plate);
      } else {
        // 번호 미확정 칸: 직전 체크의 읽기와 일치해야 인정 (연속성 = 진짜 판의 지문)
        present = SimilarPlate(btxt, presence_txt_[ch][pr.id]);
        presence_txt_[ch][pr.id] = btxt;
      }
    }
    if (present) {
      parking_.PresenceSeen(ch, pr.id, now_ms);
      if (now_ms - presence_hold_ms_[ch] >= 10000) {   // 유지 로그는 10초 스로틀
        presence_hold_ms_[ch] = now_ms;
        char m[224];
        snprintf(m, sizeof(m),
                 "🅿️ %s 추적 끊김(WiseAI 정지차 드랍)이지만 칸에 판 잔존 — 주차 유지 "
                 "(\"%s\" conf %.2f)",
                 pr.id.c_str(), btxt.c_str(), best);
        EmitEvent(ch, m);
      }
    } else {
      int miss = parking_.PresenceMiss(ch, pr.id);
      if (miss >= park_tune_.presence_miss) {
        if (parking_.ForceLeave(ch, pr.id, now_ms)) {
          presence_txt_[ch].erase(pr.id);
          char lm[128];
          snprintf(lm, sizeof(lm), "🅿️ LEFT %s — 부재 + 칸 육안검증 %d연속 빈칸, 출차 확정",
                   pr.id.c_str(), miss);
          EmitEvent(ch, lm);
          PurgePlateState(ch);
          CheckParkingEvent(ch);   // [EventStatus] 출차 통지
        }
      } else {
        char m[224];
        if (best > 0.0)
          snprintf(m, sizeof(m),
                   "🅿️ %s 부재 — 육안검증 불일치 (%d/%d, 읽힌 것 \"%s\" conf %.2f ≠ 칸 번호 %s)",
                   pr.id.c_str(), miss, park_tune_.presence_miss, btxt.c_str(), best,
                   pr.plate.empty() ? "미확정" : pr.plate.c_str());
        else
          snprintf(m, sizeof(m), "🅿️ %s 부재 — 육안검증 빈칸 (%d/%d)",
                   pr.id.c_str(), miss, park_tune_.presence_miss);
        EmitEvent(ch, m);
      }
    }
  }
}

// 한 프레임의 ONVIF 메타데이터(XML)를 받아 객체를 추적하고, 움직이는 객체를 감지한다.
void SampleComponent::ProcessObjects(int ch, const std::string& xml) {
  if (ch < 0 || ch >= cfg::kChannels) return;
  uint64_t tick = ++tick_[ch];

  // [진단] 객체 프레임 원본을 latch → /rawmeta 로 통째로 확인 (이벤트 알림 XML 은 제외)
  if (xml.find("VideoAnalytics") != std::string::npos) {   // = tt:Object 든 프레임만
    if (xml.find("LicensePlate") != std::string::npos) {
      last_plate_xml_[ch] = xml;   // 최우선: 번호판 객체가 든 프레임
    } else if (xml.find("Vehicle") != std::string::npos) {
      last_xml_[ch] = xml;         // fallback: 그냥 차량 프레임
    }
  }

  // [진단] 이벤트 알림(tt:Event) 누적 → /rawevents 로 "번호 이벤트" 사냥
  //   Topic~Data 구간만(타임스탬프 제외) 중복제거 후 append → 새 종류의 이벤트만 쌓임
  if (xml.find("<tt:Event>") != std::string::npos) {
    size_t ts = xml.find("<wsnt:Topic");
    size_t de = xml.find("</tt:Data>");
    if (ts != std::string::npos && de != std::string::npos && de > ts) {
      std::string ev = xml.substr(ts, de - ts + 10);
      if (raw_events_[ch].find(ev) == std::string::npos && raw_events_[ch].size() < 8000)
        raw_events_[ch] += ev + "\n\n";
    }
  }

  // ---- [2단계] 파싱: XML → SDK 무관 객체 구조 (문자열 스캔은 MetadataParser 안에) ----
  meta::Frame fr = meta::Parser::Parse(xml, frame_w_[ch], frame_h_[ch]);
  frame_w_[ch] = fr.frame_w;   // scale 있으면 갱신, 없으면 이전 값 유지
  frame_h_[ch] = fr.frame_h;
  double fw = frame_w_[ch] > 1.0 ? frame_w_[ch] : 1.0;
  double fh = frame_h_[ch] > 1.0 ? frame_h_[ch] : 1.0;

  // ---- 번호판 숫자 텍스트가 실려오면 알림 (카메라가 주면) ----
  if (!fr.plate_number.empty() && fr.plate_number != last_plate_) {
    last_plate_ = fr.plate_number;
    EmitEvent(ch, std::string("plate number! [") + fr.plate_number + "]");
  }

  // ---- 파싱 결과를 도메인 목록으로 분류 (detect on/off 필터는 여기서) ----
  struct Cur { long id; float cx, cy; float l, t, r, b; bool veh; };
  struct PlateBox { long id; long parent; float l, t, r, b; std::string imgref; };
  std::vector<Cur> cur;
  std::vector<PlateBox> plates;
  for (auto& ob : fr.objects) {
    if (ob.cls == meta::kPlate) {
      if (!ob.imgref.empty()) last_imgref_[ch] = ob.imgref;  // [진단] 크롭 경로 latch (detect 무관)
      if (detect_vehicle_)
        plates.push_back({ob.id, ob.parent, ob.l, ob.t, ob.r, ob.b, ob.imgref});
    } else if (ob.cls == meta::kVehicle && detect_vehicle_) {
      cur.push_back({ob.id, ob.cx, ob.cy, ob.l, ob.t, ob.r, ob.b, true});
    } else if (ob.cls == meta::kHuman && detect_person_) {
      cur.push_back({ob.id, ob.cx, ob.cy, ob.l, ob.t, ob.r, ob.b, false});
    }
    // (좌표 스케일 자동 감지는 MotionTracker 가 내부에서 처리)
  }

  // (객체 파싱은 위 MetadataParser 로 이동됨 — 여기선 분류된 cur/plates 만 사용)

  // [WiseAI 감지 상태 표시] 번호판 객체 수신 시작/끊김 전이를 디버거에 명시 —
  //   "인식하고 있는가"를 추측 말고 눈으로 확인 (08-06 요청).
  {
    uint64_t now_tmp = NowMs();
    if (!plates.empty()) {
      if (!plate_meta_on_[ch])
        EmitEvent(ch, "🔎 WiseAI plate detection ON — plate objects arriving");
      plate_meta_on_[ch] = true;
      last_plate_meta_ms_[ch] = now_tmp;
    } else if (plate_meta_on_[ch] && now_tmp - last_plate_meta_ms_[ch] > 3000) {
      plate_meta_on_[ch] = false;
      EmitEvent(ch, "🔎 WiseAI plate detection OFF — no plate objects for 3s");
    }
  }

  // 첫 프레임 진단 로그
  if (!meta_diag_done_[ch]) {
    meta_diag_done_[ch] = true;
    char d[160];
    snprintf(d, sizeof(d), "[diag] ch%d objects %zu, frame=%.0fx%.0f",
             ch, cur.size(), fw, fh);
    EmitEvent(ch, d);
  }

  // ---- [3단계] 움직임 판정 (MotionTracker) → 전환 이벤트 + 움직이는 박스 ----
  uint64_t now_ms = NowMs();
  std::vector<MotionTracker::Sample> samples;
  samples.reserve(cur.size());
  for (auto& c : cur) samples.push_back({c.id, c.cx, c.cy, c.veh});
  auto motion = motion_[ch].Update(tick, now_ms, samples);

  std::vector<Detection> dets;
  for (size_t i = 0; i < cur.size(); i++) {
    if (motion[i].just_started) {  // "정지→움직임" 전환 순간에만 이벤트 1번
      const char* cls = motion[i].is_vehicle ? "vehicle" : "person";
      char m[112];
      snprintf(m, sizeof(m), "moving %s! (ch%d id%ld)", cls, ch, cur[i].id);
      EmitEvent(ch, m);  // "moving vehicle/person! (chN idM)"
    }
    if (motion[i].moving)  // 움직이는 객체만 화면에 표시 (정지 객체는 박스 안 그림)
      dets.push_back({cur[i].id, true, cur[i].l, cur[i].t, cur[i].r, cur[i].b, false});
  }

  // ---- [주차] 정지한 차량 bbox 를 구역과 대조 (75% 겹침 + 3초 → 주차) ----
  if (parking_.size() > 0) {
    std::vector<ParkingZone::Vehicle> pv;
    for (size_t i = 0; i < cur.size(); i++) {
      if (!cur[i].veh) continue;                    // 차량만
      double L = cur[i].l, T = cur[i].t, R = cur[i].r, B = cur[i].b;
      if (R > 1.5 || B > 1.5) { L /= fw; R /= fw; T /= fh; B /= fh; }  // 픽셀→정규화
      pv.push_back({cur[i].id, L, T, R, B, !motion[i].moving});         // 정지 = !moving
    }
    // 번호판 중심점 — 차량 객체가 목록에서 빠진 장기 정지차의 점유 증거 (정규화)
    // [유령 필터] 출차한 차의 번호판 id(명부 15초)는 제외 — 얼어붙은 반복 레코드가
    //   빈칸을 재점화하는 것 방지. 새 차의 판은 새 id 라 즉시 통과 (시간 차단 없음).
    for (auto it = ghost_plate_[ch].begin(); it != ghost_plate_[ch].end();)
      it = (now_ms - it->second > 15000) ? ghost_plate_[ch].erase(it) : std::next(it);
    std::vector<ParkingZone::PlatePt> ppts;
    for (auto& p : plates) {
      if (ghost_plate_[ch].count(p.id)) continue;
      double cx = (p.l + p.r) / 2.0, cy = (p.t + p.b) / 2.0;
      if (p.r > 1.5 || p.b > 1.5) { cx /= fw; cy /= fh; }
      if (cx > 0.0 || cy > 0.0) ppts.push_back({p.id, cx, cy});  // (0,0) 무좌표 프레임 제외
    }
    std::vector<std::string> plog;
    parking_.Update(ch, pv, now_ms, &plog, &ppts);
    for (const auto& line : plog) {
      EmitEvent(ch, line);  // 디버거뷰어: 후보/주차/출차 전이
      // 출차/스쳐간 정리 순간 — 이전 차의 판독 상태를 통째로 폐기 (표 이월 금지)
      if (line.find("LEFT") != std::string::npos || line.find("스쳐간") != std::string::npos)
        PurgePlateState(ch);
    }
    CheckParkingEvent(ch);  // [EventStatus] 출차로 위반 해제됐으면 통지
    // 진입 로그 (상승엣지 + 5초 스로틀) — 판독 가동 자체는 아래 번호판 루프가 위치로 판단.
    bool attn = parking_.Attention(ch);
    if (attn && !park_attn_[ch] && now_ms - park_attn_log_ms_[ch] >= 5000) {
      EmitEvent(ch, "🅿️ zone entry (65%+) — plate read engaged");
      park_attn_log_ms_[ch] = now_ms;
    }
    park_attn_[ch] = attn;
  }

  // ---- 번호판: 감지 알림 + 저장(PlateStore) + 박스 표시 ----
  // [판독 게이트] 구역 채널은 "읽을 일이 있을 때"만 풀가동 — 후보 카운트다운 중이거나
  //   번호/EV 미확정 점유칸이 있을 때. 전 칸 완결이면 완전 침묵 (08-04).
  const bool park_busy = parking_.Busy(ch);            // 진입 로그용
  const bool park_read = parking_.NeedsRead(ch);       // 판독 가동 여부
  // [실험 종료 08-06] 굿샷 우선 5초 유예 폐지 — 굿샷(q45, sharp 300~600)이 유예 안에
  //   도착하면 오독 FINAL 을 선점하는 역효과 실측 (09러5673/03저3449/10조5466/27마8887,
  //   같은 차를 버스트는 0.96+ 정독). 버스트 즉시 가동이 정답.
  const bool burst_ok = park_read;
  for (auto& p : plates) {
    bool is_new = plate_seen_[ch].find(p.id) == plate_seen_[ch].end();
    plate_seen_[ch][p.id] = {tick, now_ms};
    // [위치 기록] 이 번호판의 중심좌표(정규화) — 배정 시 1차 참고 (실패 시 근접 폴백).
    bool settled = false;  // 이 번호판이 "완결된 칸" 안 — 판독 침묵 (15초마다 재확인 창)
    bool near_zone = true; // 칸 근처(20% 확장) 판만 판독 — 먼 판(모니터 영상 등) 제외
    {
      double cx = (p.l + p.r) / 2.0, cy = (p.t + p.b) / 2.0;
      if (p.r > 1.5 || p.b > 1.5) { cx /= fw; cy /= fh; }  // 픽셀 좌표계면 정규화
      if (cx > 0.0 || cy > 0.0) {
        plate_pos_[ch][p.id] = {cx, cy};
        if (plate_pos_[ch].size() > 512) plate_pos_[ch].clear();  // 폭주 방지
        settled = parking_.SettledAt(ch, cx, cy);
        if (parking_.HasZones(ch))
          near_zone = parking_.NearAnyZone(ch, cx, cy, park_tune_.burst_margin);
      }
      if (is_new && parking_.HasZones(ch)) {  // [진단] 좌표계 편차 확인용 1회 로그
        char pm[128];
        snprintf(pm, sizeof(pm), "  plate pos ch%d id%ld (%.2f,%.2f) in_zone=%d busy=%d",
                 ch, p.id, cx, cy, parking_.PointInAnyZone(ch, cx, cy) ? 1 : 0, park_busy ? 1 : 0);
        EmitEvent(ch, pm);
      }
    }
    if (is_new) {  // 새 번호판 등장 순간에만 알림 1번 + 진단 카운트
      plates_seen_++;
      if (!p.imgref.empty()) { imgref_seen_++; imgref_ch_[ch]++; }
      char m[128];
      snprintf(m, sizeof(m),
               "plate detected (ch%d id%ld) imgref=%s",
               ch, p.id, p.imgref.empty() ? "X" : "O");
      EmitEvent(ch, m);
    }

    // ★ 저장: 카메라 크롭(ImageRef)이 새로 오면 PlateStore 가 읽어 저장(알림 문자열 반환).
    //   [주차 전용 모드] 읽을 일이 있을 때만 저장·판독 — 평시엔 갤러리도 안 쌓는다 (B안).
    if (park_read) {
      std::string sev = plate_store_.Save(ch, p.imgref, now_ms);
      if (!sev.empty()) {
        EmitEvent(ch, sev);
        last_save_ms_[ch] = now_ms;  // [세션 경계] 이번 세션 크롭 표시
        futile_slot_[ch] = -1;       // 새 크롭 — 헛수고 마킹 해제 (새 차일 수 있음)
        RecognizePlate(ch, plate_store_.last_slot(ch));
      }
    }

    // 번호판 박스는 감지되면 무조건 표시 — "정지하면 인식 안 하는 듯" 착시 방지
    //   (판독은 정지 중에도 돈다. 카메라 메타데이터에 있는 한 초록 박스가 떠 있어야
    //    앱이 보고 있음을 눈으로 확인 가능. 08-04)
    dets.push_back({p.id, false, p.l, p.t, p.r, p.b, true});  // plate=true

    // [시간축 버스트] 읽을 일이 있을 때만 — 굿샷 우선 5초 유예 후(burst_ok) + 칸 근처 판만.
    if (!settled && burst_ok && near_zone)
      BurstSample(ch, p.id, p.l, p.t, p.r, p.b, now_ms, park_read);
  }

  // [주차 유지보수 — 전 채널] 어느 채널 프레임이 오든, 주차 진행중인 모든 채널의
  //   판독을 굴린다. 완전 정지 장면은 그 채널 메타데이터가 끊겨 자기 힘으로 못 돌기
  //   때문 (08-04 실측: ch0 정지 후 584초 동면 — 재판독·교정 전부 정지).
  for (int c = 0; c < cfg::kChannels; ++c) {
    // ⓪a [출차 육안검증] 부재 의심 칸 — NeedsRead 게이트보다 먼저 (완결 칸도 대상).
    //    출차 확정은 여기서만 내려온다 (부재 타이머 단독으로는 절대 출차 안 함).
    if (now_ms - presence_ms_[c] >= park_tune_.presence_period_ms) {
      auto susp = parking_.AbsenceSuspects(c, now_ms);
      if (!susp.empty()) {
        presence_ms_[c] = now_ms;
        PresenceCheck(c, susp, now_ms);
      }
    }
    if (!parking_.NeedsRead(c)) continue;   // 읽을 일 없는 채널은 완전 침묵
    // ⓪ [진단 심장박동] 판독 재료 현황 — "카메라가 번호판을 주는가"가 한눈에 (5초)
    if (now_ms - hb_ms_[c] >= 5000) {
      hb_ms_[c] = now_ms;
      uint64_t last_plate_ms = 0;
      int max_votes = 0;
      for (const auto& kv : plate_seen_[c]) {
        if (kv.second.ms > last_plate_ms) last_plate_ms = kv.second.ms;
        int n = plate_vote_.Count(c, kv.first);
        if (n > max_votes) max_votes = n;
      }
      char hb[224];
      if (plate_seen_[c].empty())
        snprintf(hb, sizeof(hb),
                 "🅿️ hb ch%d: CAMERA GIVES NO PLATE OBJECTS — waiting for plate bbox "
                 "(last save %.1fs ago)",
                 c, last_save_ms_[c] ? (now_ms - last_save_ms_[c]) / 1000.0 : -1.0);
      else {
        // [진단] 추적 중 판이 구역 근처(버스트 사거리)에 몇 개인가 — 0/N 이면
        //   "판은 오는데 전부 구역 밖" = 구역을 판 포함하게 다시 그려야 하는 상황.
        int near_cnt = 0, pos_cnt = 0;
        for (const auto& kv : plate_seen_[c]) {
          auto pp = plate_pos_[c].find(kv.first);
          if (pp == plate_pos_[c].end()) continue;
          pos_cnt++;
          if (parking_.NearAnyZone(c, pp->second.first, pp->second.second,
                                   park_tune_.burst_margin)) near_cnt++;
        }
        snprintf(hb, sizeof(hb),
                 "🅿️ hb ch%d: plates tracked=%d (last seen %.1fs ago, near-zone %d/%d), "
                 "votes max=%d, last save %.1fs ago",
                 c, (int)plate_seen_[c].size(),
                 last_plate_ms ? (now_ms - last_plate_ms) / 1000.0 : -1.0,
                 near_cnt, pos_cnt, max_votes,
                 last_save_ms_[c] ? (now_ms - last_save_ms_[c]) / 1000.0 : -1.0);
      }
      EmitEvent(c, hb);
    }
    // ① 밀린 판독 회수 — Busy 직전에 저장돼 스킵된 쨍한 크롭 (정지차의 유일한 표)
    if (pending_ocr_slot_[c] >= 0) {
      int s = pending_ocr_slot_[c];
      pending_ocr_slot_[c] = -1;
      RecognizePlate(c, s);
    }
    // ② 재판독 — 주차됐는데 번호 없으면 1.5초마다 마지막 저장크롭 재시도.
    //   단, 이번 세션(마지막 출차 이후)에 저장된 크롭만 — 이전 차의 유물 크롭을
    //   파오면 옛 번호가 되살아난다 (08-04 실측: 흰차 크롭으로 빨간차에 49허 재배정).
    if (parking_.HasPlatelessOccupied(c) && last_save_ms_[c] > purge_ms_[c] &&
        now_ms - reread_ms_[c] >= 1500) {
      reread_ms_[c] = now_ms;
      int s = plate_store_.last_slot(c);
      if (s >= 0 && s != futile_slot_[c]) RecognizePlate(c, s);  // 해결된 차 크롭은 제외
    }
    // ②b (제거) zone-ocr 구역 수색 — 저신뢰 환각 표(경기50배7794 류)만 만들어
    //    판독을 오염시켜 뺐다 (08-06 사용자 지시). 크롭이 전무하면 재도전 라운드가
    //    굿샷 재수신·버스트 리필로만 재시도한다. (ZoneFallbackOcr 함수는 보존)
    // ②c 재도전 라운드 — 표가 쌓였는데 결론이 없다 = 교착 (오독 다수결 점거·탄창 소진).
    //    투표함 폐기 + 프레임세대 리셋 → 백지에서 재수집. 라운드마다 지터로 결과가
    //    달라져, db-rescue 가능한 판독이 이기는 라운드에서 확정된다.
    if (parking_.HasPlatelessOccupied(c) && now_ms - retry_ms_[c] >= 4000) {
      retry_ms_[c] = now_ms;
      int discarded = 0;
      for (const auto& kv : plate_seen_[c]) {
        if (plate_done_[c].count(kv.first)) continue;
        if (plate_vote_.Count(c, kv.first) >= 2) {
          int n; double cf; bool pm;
          plate_vote_.Finalize(c, kv.first, &n, &cf, &pm);  // 폐기 (결과 버림)
          burst_ver_[c].erase(kv.first);
          discarded++;
        }
      }
      // [good-shot 재수신] 크롭 중복제거 기억도 지운다 — 메타데이터에 계속 실려오는
      //   같은 ImageRef 를 새것처럼 다시 저장·판독 (WiseAI 재요청 API 의 대체, 08-05).
      //   단 마지막 크롭이 "이미 배정된 번호"로 판명(futile)됐으면 재수신 무의미 — 스킵.
      bool futile = plate_store_.last_slot(c) == futile_slot_[c] && futile_slot_[c] >= 0;
      if (!futile) plate_store_.ForgetRefs(c);
      // [확정딱지 사면] 굶주리는데 추적 중인 번호판이 전부 plate_done 이면 — 이전 구역
      //   세션의 딱지가 유일한 재료를 막고 있는 것 (구역 재생성 후 교착 실측 08-06).
      //   칸이 번호를 기다리는 지금은 사면하고 다시 읽게 한다.
      if (discarded == 0 && !plate_seen_[c].empty() && !plate_done_[c].empty()) {
        bool all_done = true;
        for (const auto& kv : plate_seen_[c])
          if (!plate_done_[c].count(kv.first)) { all_done = false; break; }
        if (all_done) {
          plate_done_[c].clear();
          EmitEvent(c, "🅿️ read starving — done-marks amnestied (reopen tracked plates)");
        }
      }
      if (discarded)
        EmitEvent(c, "🅿️ read stuck — ballots discarded + crop dedup reset, fresh round");
      else if (!futile)
        EmitEvent(c, "🅿️ read starving — crop dedup reset (re-ingest good-shot)");
      // futile 상태면 침묵 — zone-ocr 만 남은 상황이고 그건 자기 로그가 있다
    }
    // ③ 조기개표 — 신뢰도 차는 순간 FINAL → 배정 (좌표 1차, 실패 시 빈칸 우선 폴백)
    //    (주기 재검증(10초 사면)은 폐지 — 완결 칸은 출차까지 완전 침묵이 원칙. 08-04)
    TryEarlyFinalize(c, now_ms);
  }

  // 늦게 써진 크롭 파일 재시도 (PlateStore, 최대 3초)
  std::string rev = plate_store_.RetryPending(ch, now_ms);
  if (!rev.empty()) {
    EmitEvent(ch, rev);
    last_save_ms_[ch] = now_ms;  // [세션 경계] 이번 세션 크롭 표시
    RecognizePlate(ch, plate_store_.last_slot(ch));
  }

  latest_[ch].swap(dets);  // 웹 오버레이가 읽어감 (움직이는 객체 + 그 번호판)

  // 오래 안 보인 번호판 확정 — 전 채널을 훑는다. 영상이 끝난 채널은 메타데이터가
  // 끊겨 자기 tick 이 못 굴러가므로, 살아있는 다른 채널의 프레임이 시계를 대신 돌려준다.
  for (int c = 0; c < cfg::kChannels; ++c) FinalizeStalePlates(c, now_ms);
  motion_[ch].PruneStale(tick);  // 오래 안 보인 객체 추적 제거
}

// ---- DB 매칭 레이어 (stale/조기개표 공용) ----
//   HOLD 회수: 등록 번호와 정확 일치(conf≥0.85) 또는 유일 1글자 차이(conf≥0.90)
//   → FINAL 로 승격. FINAL 교정: 미등록 텍스트가 유일 1글자 매칭이면 등록 번호로.
//   복수 매칭·매칭 없음은 불개입 (미등록 차량은 그대로 통과/억류).
const char* SampleComponent::ApplyDbLayer(std::string* fin, double fconf, bool* trusted) {
  if (!plate_db_ready_) return "";
  std::string dbtxt;
  int m = plate_db_.Match(*fin, &dbtxt);
  if (m == 1) {
    bool exact = (dbtxt == *fin);
    if (*trusted) {
      if (!exact) { *fin = dbtxt; return ", db-fix"; }
      return "";
    }
    if ((exact && fconf >= cfg::kDbRescueExactMin) ||
        (!exact && fconf >= cfg::kDbRescueEd1Min)) {
      *fin = dbtxt;
      *trusted = true;
      return exact ? ", db-rescue" : ", db-rescue-fix";
    }
    return "";
  }
  if (m == 2) return "";  // 1글자 복수 매칭 — 애매, 불개입
  // [tier-2] 1글자 그물 밖 — 2글자 치환 유일 매칭 회수 (저해상 혼동쌍 오독 전용).
  //   사람 눈이 문맥으로 복원하듯, 등록차 목록이 모델의 문맥이 된다 (08-06 도입).
  if (plate_db_.MatchLoose(*fin, &dbtxt) == 1 && fconf >= 0.90) {
    const char* tag = *trusted ? ", db-fix2" : ", db-rescue2";
    *fin = dbtxt;
    *trusted = true;
    return tag;
  }
  return "";
}

// [주차 조기개표] 구역이 번호를 기다리는 동안 매 프레임 호출 — 추적 종료를 기다리지
//   않고 지금까지의 투표함을 심사(Peek). 신뢰도가 차면 그 자리에서 FINAL 확정
//   (RecordFinal → 구역 배정 + EV 판정까지 기존 경로 그대로). 아직 부족하면
//   투표함을 건드리지 않아 버스트 샘플이 계속 쌓인다 — "차가 있는 동안 계속 시도".
void SampleComponent::TryEarlyFinalize(int ch, uint64_t now_ms) {
  for (const auto& kv : plate_seen_[ch]) TryFinalizeOid(ch, kv.first, now_ms);
}

// 한 대(oid)만 즉석 심사 — 표 추가 직후 호출 (plate_seen_ 목록에 없어도 동작).
//   정지 장면에선 추적 목록이 비어 조기개표 루프가 투표함을 영영 안 열었다 (08-04
//   실측: 재판독 0.99 표가 4장 쌓여도 무결론). 넣는 손이 바로 개표까지 한다.
bool SampleComponent::TryFinalizeOid(int ch, long oid, uint64_t now_ms) {
  if (plate_done_[ch].count(oid)) return false;   // 이미 확정된 차
  int nvotes = 0; double fconf = 0.0; bool fprim = false;
  std::string fin = plate_vote_.Peek(ch, oid, &nvotes, &fconf, &fprim);
  if (fin.empty()) return false;
  if (nvotes < 2) return false;  // [합의제] 단발 오독 방지 — 연사 중이라 2표는 ~0.6초
  bool trusted = fconf >= cfg::kFinalConfFloor;
  const char* dbtag = ApplyDbLayer(&fin, fconf, &trusted);
  if (!trusted) return false;                     // 증거 부족 — 다음 표에서 재심사
  plate_vote_.Finalize(ch, oid, &nvotes, &fconf, &fprim);  // 확정 — 이제 투표함 비움
  // [완결 재확인] 이 번호가 이미 이 채널 칸에 붙어 있으면 — 조용히 갱신만 하고 끝.
  //   (재검증 창에서 같은 번호 재확인 = 정상. FINAL/EV 로그 도배 방지, 08-04 실측)
  if (parking_.HasPlate(ch, fin)) {
    plate_done_[ch].insert(oid);
    auto pp = plate_pos_[ch].find(oid);
    if (pp != plate_pos_[ch].end())
      parking_.AssignAt(ch, pp->second.first, pp->second.second, fin, -1, "", "", now_ms);
    return true;
  }
  last_final_[ch] = fin;
  RecordFinal(ch, fin, now_ms, oid);              // 구역 배정 + EV 연쇄
  plate_done_[ch].insert(oid);                    // stale 경로의 이중판정 방지
  char m[256];
  snprintf(m, sizeof(m), "%s★ PLATE FINAL ch%d id%ld -> \"%s\" (early/parked, best-of-%d, conf %.2f, src=%s%s)%s",
           Hl(cfg::kAnsiFinal), ch, oid, fin.c_str(), nvotes, fconf,
           fprim ? "good-shot" : "burst", dbtag, Hl(cfg::kAnsiReset));
  EmitEvent(ch, m);
  return true;
}

// 만료된 번호판을 신뢰도 챔피언십으로 최종 확정한다 (FINAL/HOLD).
//   [②버스트 승격] good-shot/버스트 무관, 유효포맷 중 conf 최고가 승리 (Q45 우회 크롭의 역전 허용)
//   만료 = 그 채널 프레임 기준(kStaleFrames) OR 벽시계 기준(kStaleMs) — 채널이 조용해져도 확정됨.
void SampleComponent::FinalizeStalePlates(int ch, uint64_t now_ms) {
  uint64_t tick = tick_[ch];
  for (auto it = plate_seen_[ch].begin(); it != plate_seen_[ch].end();) {
    // good-shot 크롭은 최대 3초 늦게 써짐(RetryPending) → primary 샘플이 아직 없으면
    // stale 유예를 3배로 늘려 기다린다 (버스트 환각만으로 조기 오답 FINAL 방지, id9595 실측)
    bool has_primary = plate_vote_.HasPrimary(ch, it->first);
    uint64_t grace    = has_primary ? cfg::kStaleFrames : cfg::kStaleFrames * 3;
    uint64_t grace_ms = has_primary ? cfg::kStaleMs : cfg::kStaleMs * 3;
    if (it->second.tick + grace < tick || it->second.ms + grace_ms < now_ms) {
      // 즉시확정으로 이미 판정이 끝난 차 — 조용히 정리만 (이중판정 방지)
      if (plate_done_[ch].erase(it->first)) {
        stack_acc_[ch].erase(it->first);
        it = plate_seen_[ch].erase(it);
        continue;
      }
      // [스태킹] 확정 직전, 이 차의 버스트 평균본(노이즈 √N 상쇄)으로 마지막 1표 시도
      auto sa = stack_acc_[ch].find(it->first);
      if (sa != stack_acc_[ch].end()) {
        if (sa->second.n >= cfg::kStackMinFrames && plate_ocr_ready_) {
          cv::Mat avg8;
          sa->second.sum.convertTo(avg8, CV_8U, 1.0 / sa->second.n);
          PlateOcrResult sr = plate_ocr_.Recognize(avg8);
          if (plate_decode::ValidPlateFormat(sr.text)) {
            // 평균본도 가공 → conf 0.95 캡 (합의 표로만 기여, 단독 확정 불가)
            plate_vote_.Add(ch, it->first, sr.text, std::min(sr.confidence, 0.95),
                            /*primary=*/false);
            char sm[192];
            snprintf(sm, sizeof(sm), "  stack ch%d id%ld avg-of-%d -> \"%s\" (conf %.2f, %.1fms)",
                     ch, it->first, sa->second.n, sr.text.c_str(), sr.confidence, sr.total_ms);
            EmitEvent(ch, sm);
          }
        }
        stack_acc_[ch].erase(sa);
      }

      int nvotes = 0; double fconf = 0.0; bool fprim = false;
      std::string fin = plate_vote_.Finalize(ch, it->first, &nvotes, &fconf, &fprim);
      if (!fin.empty()) {
        // conf 하한 미달이면 FINAL 대신 HOLD — 시스템에 확정값으로 넘기지 않고 로그만 남긴다.
        bool trusted = fconf >= cfg::kFinalConfFloor;
        const char* dbtag = ApplyDbLayer(&fin, fconf, &trusted);  // DB 교정/회수 (공용)
        if (trusted) { last_final_[ch] = fin; RecordFinal(ch, fin, now_ms, it->first); }
        char m[256];
        snprintf(m, sizeof(m), "%s%s PLATE %s ch%d id%ld -> \"%s\" (best-of-%d, conf %.2f, src=%s%s)%s",
                 Hl(trusted ? cfg::kAnsiFinal : cfg::kAnsiWarn), trusted ? "★" : "?",
                 trusted ? "FINAL" : "HOLD ", ch, it->first, fin.c_str(), nvotes, fconf,
                 fprim ? "good-shot" : "burst", dbtag, Hl(cfg::kAnsiReset));
        EmitEvent(ch, m);
        // FINAL 로 끝난 차는 늦게 도착하는 good-shot 재판독을 막는다 (이중 FINAL 방지).
        // HOLD 는 일부러 안 막음 — 늦은 good-shot 1.00 이 오면 승격 기회를 준다.
        if (trusted) {
          plate_done_[ch].insert(it->first);
          if (plate_done_[ch].size() > 256) plate_done_[ch].clear();  // oid 는 재사용 안 됨 — 폭주만 방지
        }
      }
      it = plate_seen_[ch].erase(it);
    } else {
      ++it;
    }
  }
}

extern "C" {
SampleComponent* create_component(void* mem_manager) {
  Component::allocator = decltype(Component::allocator)(mem_manager);
  Event::allocator = decltype(Event::allocator)(mem_manager);
  return new ("SampleComponent") SampleComponent();
}

void destroy_component(SampleComponent* ptr) { delete ptr; }
}
