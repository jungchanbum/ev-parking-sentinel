#include "sample_component.h"

#include <dirent.h>                     // [④탐사] /lsdownload 디렉터리 나열
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
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

// 현재 시각(ms). 움직임 3초 유지 타이머용.
uint64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// (XML 파싱 헬퍼 FindAttr/Trim/ExtractPlate 는 [2단계]에서 core/metadata_parser.h 로 이동)

}  // namespace

SampleComponent::SampleComponent() : SampleComponent(_SampleComponent_Id, "SampleComponent") {}

SampleComponent::SampleComponent(ClassID id, const char* name) : Component(id, name) {}

SampleComponent::~SampleComponent() {
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

  RegisterURI();                     // /detections HTTP 엔드포인트 등록

  // 번호판 OCR 모델 로드(tinyLPR 단독 — Multi-line 은 기여 0 실측으로 제거).
  // 실패해도 앱은 계속 동작(크롭 저장은 그대로).
  plate_ocr_ready_ = plate_ocr_.Load(cfg::kKrLprModel, cfg::kKrLprLabels);
  printf("[object_detect] plate OCR model %s\n", plate_ocr_ready_ ? "loaded" : "FAILED to load");

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

// 방금 PlateStore 가 저장한 cap_<slot>.jpg 를 다시 읽어 번호판 숫자를 인식.
//   PC(ocr_lab)에서 검증한 tinyLPR+Multi-line 신뢰도 병합을 그대로 사용.
//   디버그 뷰어에 단계별 지연시간(load/tiny/multi/total)까지 찍어 병목을 드러낸다.
void SampleComponent::RecognizePlate(int ch, int slot) {
  if (!plate_ocr_ready_) {
    EmitEvent(ch, "OCR skipped: models not loaded");
    return;
  }
  if (ch < 0 || ch >= cfg::kChannels || slot < 0) return;

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

  // 초소형 크롭(먼 차)은 환각만 만들므로 스킵 — 실측 근거는 config.h 주석 참고.
  if (img.cols < cfg::kMinOcrCropWidth) {
    char m[112];
    snprintf(m, sizeof(m), "OCR skipped ch%d #%d: crop too small (%dx%d < %dpx)",
             ch, slot, img.cols, img.rows, cfg::kMinOcrCropWidth);
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
  snprintf(m0, sizeof(m0), "OCR start ch%d #%d crop=%dx%d sharp %.0f (load %.1fms)",
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
  snprintf(m1, sizeof(m1), "  tinyLPR(%d cands): \"%s\" (conf %.2f, %.1fms) | lum %s | color %s | rescue %.1fms",
           r.cand_total, r.tiny_text.c_str(), r.tiny_conf, r.tiny_ms, lumbuf,
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
  if (oid != 0 && plate_decode::ValidPlateFormat(r.text)) {
    plate_vote_.Add(ch, oid, r.text, r.confidence,
                    /*primary=*/sharp >= cfg::kGoodshotSharpMin);
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
                                  uint64_t now_ms) {
  if (!plate_ocr_ready_ || r <= l || b <= t) return;
  if (!plate_vote_.CanSample(ch, oid, now_ms)) return;

  RefreshSnapshot(ch);  // 스냅샷 주문(스로틀) + 캐시 갱신
  std::string jpg;
  { std::lock_guard<std::mutex> lk(jpeg_mtx_); jpg = last_jpeg_[ch]; }
  if (jpg.empty()) return;

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
  PlateOcrResult br = plate_ocr_.Recognize(crop);
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

  // ---- 번호판: 감지 알림 + 저장(PlateStore) + 박스 표시 ----
  for (auto& p : plates) {
    bool is_new = plate_seen_[ch].find(p.id) == plate_seen_[ch].end();
    plate_seen_[ch][p.id] = {tick, now_ms};
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
    //   imagetransfer 켜진 채널이면 4채널 모두 ImageRef 가 오므로 그대로 처리됨.
    std::string sev = plate_store_.Save(ch, p.imgref, now_ms);
    if (!sev.empty()) { EmitEvent(ch, sev); RecognizePlate(ch, plate_store_.last_slot(ch)); }

    // 번호판 박스 표시 (부모 차량 추적이 없으면 무조건, 있으면 움직일 때만)
    bool show = !motion_[ch].IsTracked(p.parent) || motion_[ch].IsMoving(p.parent);
    if (show) dets.push_back({p.id, false, p.l, p.t, p.r, p.b, true});  // plate=true

    // [시간축 버스트] 추적 프레임(실제 bbox 있는 것)마다 스냅샷 크롭 샘플 시도
    BurstSample(ch, p.id, p.l, p.t, p.r, p.b, now_ms);
  }

  // 늦게 써진 크롭 파일 재시도 (PlateStore, 최대 3초)
  std::string rev = plate_store_.RetryPending(ch, now_ms);
  if (!rev.empty()) { EmitEvent(ch, rev); RecognizePlate(ch, plate_store_.last_slot(ch)); }

  latest_[ch].swap(dets);  // 웹 오버레이가 읽어감 (움직이는 객체 + 그 번호판)

  // 오래 안 보인 번호판 확정 — 전 채널을 훑는다. 영상이 끝난 채널은 메타데이터가
  // 끊겨 자기 tick 이 못 굴러가므로, 살아있는 다른 채널의 프레임이 시계를 대신 돌려준다.
  for (int c = 0; c < cfg::kChannels; ++c) FinalizeStalePlates(c, now_ms);
  motion_[ch].PruneStale(tick);  // 오래 안 보인 객체 추적 제거
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
        if (trusted) last_final_[ch] = fin;
        char m[256];
        snprintf(m, sizeof(m), "%s%s PLATE %s ch%d id%ld -> \"%s\" (best-of-%d, conf %.2f, src=%s)%s",
                 Hl(trusted ? cfg::kAnsiFinal : cfg::kAnsiWarn), trusted ? "★" : "?",
                 trusted ? "FINAL" : "HOLD ", ch, it->first, fin.c_str(), nvotes, fconf,
                 fprim ? "good-shot" : "burst", Hl(cfg::kAnsiReset));
        EmitEvent(ch, m);
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
