#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "component.h"
#include "i_sample_component.h"
#include "io/i_event_sink.h"       // [1단계] 이벤트 출구 이음새
#include "core/motion_tracker.h"   // [3단계] 채널별 움직임 추적
#include "core/plate_store.h"      // [3단계] 번호판 크롭 저장
#include "ocr/plate_db.h"          // 등록차량 목록 최근접 매칭 (HOLD 회수·FINAL 교정)
#include "ocr/plate_ocr.h"         // 번호판 숫자 인식(tinyLPR+Multi-line 신뢰도 병합)
#include "ocr/plate_vote.h"        // 시간축(버스트) 샘플 누적 + 자리별 다수결

class SampleComponent : public Component, public ISampleComponent {
 public:
  SampleComponent();
  SampleComponent(ClassID id, const char* name);
  virtual ~SampleComponent();
  bool ProcessAEvent(Event* event) override;

 protected:
  bool Initialize() override;

 private:
  // 메타데이터 수신 → 파싱 → 움직이는 객체 감지 → 이벤트 출력
  void HandleMetadataEvent(Event* event);
  void ProcessObjects(int channel, const std::string& json);
  void EmitEvent(int channel, const std::string& msg);

  // 웹페이지(Go App)용 HTTP: /detections?ch=N → 현재 감지 박스 JSON
  void RegisterURI();
  bool HandleHttpRequest(Event* event);

  // 웹 오버레이용: 현재 프레임의 감지 박스(정규화 0~1)
  struct Detection {
    long id;
    bool moving;
    float l, t, r, b;  // left, top, right, bottom (0~1)
    bool plate = false;  // 번호판 박스면 true (초록/다른 색으로 그림)
  };

  // 카메라에 스냅샷 1장 주문 + 완성 파일 읽어 last_jpeg_ 갱신 (라이브뷰용)
  void RefreshSnapshot(int channel);

  // [3단계] 도메인 모듈 — 움직임 추적(채널별) + 번호판 저장(파일 I/O)
  MotionTracker motion_[4];  // 채널별 움직임 추적기
  PlateStore plate_store_;   // 번호판 크롭 저장(순환버퍼·중복제거·재시도, 채널 내부관리)

  // 번호판 숫자 인식(OCR) — 새 번호판 저장 직후 그 크롭으로 1회 인식.
  PlateOcr plate_ocr_;
  bool plate_ocr_ready_ = false;                 // 모델 로드 성공 여부
  void RecognizePlate(int ch, int slot);         // cap_<slot>.jpg 읽어 인식 + 이벤트+저장
  PlateOcrResult last_plate_ocr_[4];             // 채널별 마지막 인식 결과 (HTTP /platetext)

  // [DB 매칭] 등록차량 목록 — HOLD 회수(db-rescue)·FINAL 교정(db-fix)
  PlateDb plate_db_;
  bool plate_db_ready_ = false;

  // [시간축 버스트] 추적 중 bbox+스냅샷으로 샘플 추가 캡처 → 자리별 다수결로 최종 확정
  PlateVote plate_vote_;
  void BurstSample(int ch, long oid, float l, float t, float r, float b, uint64_t now_ms);
  std::string last_final_[4];                    // 채널별 마지막 확정 번호 (HTTP /platetext)

  // [스태킹] 버스트 크롭을 고정 캔버스에 합산 누적 → 추적 종료 시 평균내어 추가 1표.
  //   노이즈는 무작위라 N장 평균 시 √N 배 상쇄, 글자는 보존 (다중 프레임 스태킹).
  struct StackAcc { cv::Mat sum; int n = 0; };
  std::map<long, StackAcc> stack_acc_[4];

  // [진단] 어디서 놓치는지 카운트 (감지 단계 — 저장은 PlateStore 가 셈)
  int plates_seen_ = 0;   // 새로 본 번호판 객체 수
  int imgref_seen_ = 0;   // 그중 ImageRef 붙은 수
  int imgref_ch_[4] = {0, 0, 0, 0};  // 채널별 ImageRef 붙은 번호판 수 (크롭이 어느 채널에 오나)

 private:
  // 감지 대상 on/off (웹 /config 로 조절). 기본 둘 다 켬.
  bool detect_person_ = true;
  bool detect_vehicle_ = true;

  std::string last_plate_;        // 마지막으로 알린 번호판 텍스트 (중복 방지)
  bool plate_raw_dumped_ = false; // 번호판 raw 포맷 확인용 1회 덤프
  std::string last_xml_[4];        // [진단] 채널별 마지막 차량 객체 프레임 XML (fallback)
  std::string last_plate_xml_[4];  // [진단] 채널별 마지막 "번호판 든 객체 프레임" XML (우선)
  std::string raw_events_[4];      // [진단] 채널별 누적 이벤트 알림(중복제거) → /rawevents
  // 번호판 id → 마지막 목격 (tick=그 채널 프레임 수, ms=벽시계).
  //   만료는 둘 중 하나로 판정 — 영상이 끝나 채널 메타데이터가 끊기면 tick 이 멈추므로
  //   벽시계가 없으면 마지막 차가 영원히 팬딩으로 남는다 (2026-07-28 실측).
  struct PlateSeen { uint64_t tick = 0; uint64_t ms = 0; };
  std::map<long, PlateSeen> plate_seen_[4];
  void FinalizeStalePlates(int ch, uint64_t now_ms);  // 만료 번호판 확정 (전 채널 대상 호출)
  // 즉시확정(good-shot 1.00)된 차 — 이후 버스트/재판독/개표를 전부 건너뛴다.
  std::set<long> plate_done_[4];

  uint64_t tick_[4] = {0, 0, 0, 0};               // 채널별 프레임 카운터
  bool meta_diag_done_[4] = {false, false, false, false};  // 첫 진단 로그 여부
  std::string last_imgref_[4];         // 마지막 번호판 ImageRef 경로 (카메라가 잘라준 크롭)

  std::string app_id_;  // 스냅샷 요청용 앱 ID (eInformAppInfo 로 수신)

  // [1단계] 이벤트가 나가는 출구 목록. 지금은 DebugViewerSink 하나뿐.
  //   나중에 HttpPushSink/RecordSink 등을 push_back 만 하면 확장(도메인 코드 무수정).
  std::vector<IEventSink*> sinks_;

  // 라이브 스냅샷 서버측 더블버퍼 (블로킹 없이 부드럽게)
  std::string last_jpeg_[4];               // 채널별 마지막 "완성된" JPEG (메모리 캐시)
  std::mutex jpeg_mtx_;                     // last_jpeg_ 동시접근 보호(http vs 메타 스레드)
  uint64_t last_snap_trigger_[4] = {0, 0, 0, 0};  // 마지막 스냅샷 주문 시각(ms)

  // 채널별 최신 감지 박스 (HTTP /detections 응답용)
  std::vector<Detection> latest_[4];
  double frame_w_[4] = {0, 0, 0, 0};   // 메타데이터 좌표계 프레임 폭(px)
  double frame_h_[4] = {0, 0, 0, 0};   // 메타데이터 좌표계 프레임 높이(px)
};
