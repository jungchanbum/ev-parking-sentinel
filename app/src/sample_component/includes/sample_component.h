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
#include "io/ev_client.h"          // [EV 실조회] ev.or.kr 저공해 확인 (미등록 번호)
#include "io/i_event_sink.h"       // [1단계] 이벤트 출구 이음새
#include "core/motion_tracker.h"   // [3단계] 채널별 움직임 추적
#include "core/parking_zone.h"     // [주차] 구역 설정·주차판정·EV위반·상태 XML
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
  // zone=true: 주차구역 안 번호판 — 웜업 없이 연사(상한 12·300ms 간격)로 존나 읽는다.
  void BurstSample(int ch, long oid, float l, float t, float r, float b, uint64_t now_ms,
                   bool zone = false);
  std::string last_final_[4];                    // 채널별 마지막 확정 번호 (HTTP /platetext)

  // [EV 판정] 최근 ★FINAL 이력 — /isev 가 "그 번호를 실제로 봤는가 + 판 색"을 조회.
  //   color 는 그 채널 마지막 good-shot 의 판 색 분류(wh/ye/gr/bl) — bl = 전기차 파란판.
  struct FinalRec { std::string text; std::string color; uint64_t ms; };
  std::deque<FinalRec> finals_log_;              // 최근 64건 (전 채널 공용)
  void RecordFinal(int ch, const std::string& text, uint64_t now_ms, long oid);

  // [위치 기반 배정] "구역 안에서 읽힌 번호는 그 구역 것" — 번호판 oid 의 마지막
  //   중심좌표(정규화)를 매 프레임 기록, FINAL 순간 그 좌표가 든 칸에 배정(AssignAt).
  //   oid 간접연결(명찰/parent/소급) 전부 폐기 — 위치가 곧 매칭 (2026-08-04 단순화).
  std::map<long, std::pair<double, double>> plate_pos_[4];
  uint64_t park_attn_log_ms_[4] = {0, 0, 0, 0};  // zone entry 로그 스로틀(5초)

  // [주차 조기개표] 구역에 번호판이 보이는 동안, 추적 종료(stale)를 기다리지 않고
  //   현재 투표함을 심사(Peek) — 신뢰도가 차면 즉시 FINAL, 아니면 계속 수집.
  void TryEarlyFinalize(int ch, uint64_t now_ms);
  // 한 대(oid)만 심사 — 표 추가 직후 즉석 개표용 (plate_seen_ 에 없어도 동작).
  bool TryFinalizeOid(int ch, long oid, uint64_t now_ms);
  // FINAL 확정 직전 DB 매칭 레이어 (stale/조기개표 공용) — fin 교정, trusted 승격.
  const char* ApplyDbLayer(std::string* fin, double fconf, bool* trusted);

  // [EV 실조회] 미등록 번호는 카메라가 직접 ev.or.kr 저공해 확인을 조회한다.
  //   워커 스레드가 조회하고 결과를 ev_results_ 에 넣으면, 메타데이터 스레드가
  //   DrainEvResults() 로 회수해 ⚡EV 이벤트를 발행 (EmitEvent 단일 스레드 규율).
  //   ev_cache_: 번호 → {판정, 상세}. 판정 1=EV, 0=아님, -2=조회중 (중복 방지).
  EvClient ev_client_;
  std::map<std::string, std::pair<int, std::string>> ev_cache_;
  std::deque<EvClient::Result> ev_results_;
  std::mutex ev_mtx_;                            // ev_cache_/ev_results_ 보호
  void DrainEvResults();

  // [주차] Pi 가 등록한 구역 + 실시간 주차/EV위반 상태. Update 는 ProcessObjects 에서,
  //   번호·EV 연결은 RecordFinal/DrainEvResults 에서. /parking/* 엔드포인트로 노출.
  ParkingZone parking_;
  // [런타임 튜닝] GET/POST /parking_tune — 판정 파라미터 즉시 조정 (빌드·설치 불필요)
  ParkTune park_tune_;
  bool park_attn_[4] = {false, false, false, false};  // 구역 진입 상승엣지 로그용

  // [EventStatus] 주차위반을 카메라 SUNAPI eventstatus.cgi 네이티브 이벤트로 노출.
  //   번들된 OpenEventDispatcher(.so)가 앱↔펌웨어 EventstatusCGIDispatcher 를 중계.
  //   스키마 요청(eEventstatusSchema/eMetadataSchema)에 응답해 이벤트 등록,
  //   위반상태 전이 시 eEventStatusChanged 통지 → Pi 가 monitordiff 로 수신.
  bool park_evt_state_[4] = {false, false, false, false};  // 채널별 마지막 통지 상태
  void SendParkingEventSchema(Event* event);   // eEventstatusSchema 응답 (JSON+TEXT)
  void SendParkingMetaSchema(Event* event);    // eMetadataSchema 응답 (ONVIF 토픽)
  // [EventStatus 벨] 의미 = "주차 이벤트" — 점유칸 생기면 True, 전부 비면 False.
  //   칸 내용(번호/EV/위반)이 바뀌었는데 벨 값이 그대로면 펄스(False→True)로 diff 라인을
  //   강제 → Pi 는 "라인이 변할 때마다 XML 열람"만 하면 됨. force_state: -1=현재값 / 0/1.
  void NotifyParkingEvent(int ch, bool check, int force_state = -1);
  void CheckParkingEvent(int ch);              // 점유/내용 전이 감지 → 통지·펄스

  void NotifyParkingSpace(int ch, int rule_idx, const ParkingZone::Snap& s, bool check);
  // [벨 2호] ParkingOccupied — 칸별(룰=칸 순번) 점유 불리언 통지
  void NotifyOccupied(int ch, int rule_idx, const std::string& space_id, bool occ, bool check);
  bool CheckParkingStatus(int ch);             // 칸별 전이 감지 (반환: 바뀐 칸 있었나)
  std::map<std::string, ParkingZone::Snap> park_snap_sent_;  // 칸 id → 마지막 통지 상태

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
  // [완전 리셋] 출차 순간 그 채널의 판독 진행상태(투표함·추적·스택) 전부 폐기 —
  //   이전 차의 표가 다음 차에 이월되어 옛 번호가 꽂히는 사고 방지 (49허5678 유령).
  void PurgePlateState(int ch);

  // [밀린 판독 회수] 후보 시작 직전에 저장돼 deferred 로 스킵된 크롭의 슬롯 —
  //   Busy 가 켜지는 순간 꺼내 읽는다 (정지차는 재저장이 없어 이게 유일한 기회).
  int pending_ocr_slot_[4] = {-1, -1, -1, -1};
  uint64_t reread_ms_[4] = {0, 0, 0, 0};  // 주차중·번호없음일 때 저장크롭 재판독 스로틀
  // [세션 경계] 재판독이 이전 차의 유물 크롭을 파오는 사고 방지 — 출차(purge) 시각과
  //   마지막 저장 시각을 비교해, 이번 세션에 저장된 크롭만 재판독 대상으로 인정.
  uint64_t purge_ms_[4] = {0, 0, 0, 0};      // 마지막 출차 정리 시각
  uint64_t last_save_ms_[4] = {0, 0, 0, 0};  // 마지막 크롭 저장 시각

  // [최후 판독] 카메라가 번호판 객체를 안 주는 정지차 — 구역 영역을 스냅샷에서
  //   직접 잘라 OCR (구역=크롭 힌트). 새 스냅샷 프레임에서만, 2초 간격.
  void ZoneFallbackOcr(int ch, uint64_t now_ms);
  uint64_t zfb_ms_[4] = {0, 0, 0, 0};
  uint64_t zfb_ver_[4] = {0, 0, 0, 0};
  // [출차 육안검증] 부재 의심 칸을 스냅샷으로 직접 확인 — 빈칸 3연속일 때만 LEFT.
  //   (WiseAI 는 정지차를 목록에서 빼버려 부재≠출차 — 08-06 가짜 출차 연발 수리)
  void PresenceCheck(int ch, const std::vector<ParkingZone::PendingRect>& rects,
                     uint64_t now_ms);
  uint64_t presence_ms_[4] = {0, 0, 0, 0};
  uint64_t presence_ver_[4] = {0, 0, 0, 0};
  uint64_t presence_hold_ms_[4] = {0, 0, 0, 0};  // "주차 유지" 로그 스로틀
  // 번호 미확정 칸의 직전 육안검증 읽기 — 연속성 대조용 (환각은 매번 다른 번호)
  std::map<std::string, std::string> presence_txt_[4];
  // [유령 명부] 출차한 차의 번호판 id → 출차 시각. WiseAI 의 얼어붙은 반복 레코드가
  //   빈칸을 재점화하지 못하게 15초간 점유 증거에서 제외 (새 id 는 즉시 통과).
  std::map<long, uint64_t> ghost_plate_[4];
  // (08-06 실험 폐기) 굿샷 우선 5초 유예 — 저품질 굿샷이 오독 FINAL 을 선점해 제거.
  // [재도전 라운드] 표가 쌓였는데 결론이 안 나는 교착(같은 오독이 다수결을 점거 +
  //   탄창 소진) — 6초마다 투표함을 폐기하고 백지에서 재수집. 라운드마다 지터로
  //   결과가 달라져 db-rescue 가능한 그룹이 이기는 라운드가 온다 (08-04).
  uint64_t retry_ms_[4] = {0, 0, 0, 0};
  uint64_t hb_ms_[4] = {0, 0, 0, 0};  // [진단] 판독 현황 심장박동 (5초)
  // [헛수고 차단] 이 슬롯의 크롭은 "이미 배정된 번호"로 판명 — 재판독 금지.
  //   (다른 칸이 번호를 기다릴 때 해결된 차의 크롭을 무한 재인식하는 사고 방지, 08-05)
  int futile_slot_[4] = {-1, -1, -1, -1};
  // [WiseAI 감지 상태] 번호판 객체 수신 여부 전이 로그용
  bool plate_meta_on_[4] = {false, false, false, false};
  uint64_t last_plate_meta_ms_[4] = {0, 0, 0, 0};
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
  uint64_t jpeg_ver_[4] = {0, 0, 0, 0};    // 스냅샷 세대 — 버스트가 같은 프레임 재탕 방지
  std::map<long, uint64_t> burst_ver_[4];  // oid 별 마지막으로 판독한 스냅샷 세대
  uint64_t last_snap_trigger_[4] = {0, 0, 0, 0};  // 마지막 스냅샷 주문 시각(ms)

  // 채널별 최신 감지 박스 (HTTP /detections 응답용)
  std::vector<Detection> latest_[4];
  double frame_w_[4] = {0, 0, 0, 0};   // 메타데이터 좌표계 프레임 폭(px)
  double frame_h_[4] = {0, 0, 0, 0};   // 메타데이터 좌표계 프레임 높이(px)
};
