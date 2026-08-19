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
#include "config.h"                // cfg::kChannels — 채널 상태 배열 크기의 단일 출처
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

  // 번호판 id → 마지막 목격 (tick=그 채널 프레임 수, ms=벽시계).
  //   만료는 둘 중 하나로 판정 — 영상이 끝나 채널 메타데이터가 끊기면 tick 이 멈추므로
  //   벽시계가 없으면 마지막 차가 영원히 팬딩으로 남는다 (2026-07-28 실측).
  struct PlateSeen { uint64_t tick = 0; uint64_t ms = 0; };

  // [스태킹] 버스트 크롭을 고정 캔버스에 합산 누적 → 추적 종료 시 평균내어 추가 1표.
  struct StackAcc { cv::Mat sum; int n = 0; };

  // ==========================================================================
  // ChState — 한 채널의 실시간 상태 전부 (구 [4] 병렬 배열 40개를 통합, 08-07 리팩터링).
  //   접근은 C(ch).멤버 — 멤버 추가/채널 수 변경이 이 구조체 한 곳으로 끝난다.
  // ==========================================================================
  struct ChState {
    // ---- 감지·추적 ----
    MotionTracker motion;                 // 움직임 추적기
    uint64_t tick = 0;                    // 프레임 카운터
    bool meta_diag_done = false;          // 첫 진단 로그 여부
    std::map<long, PlateSeen> plate_seen; // 번호판 id → 마지막 목격
    std::set<long> plate_done;            // 확정 완료 — 버스트/재판독/개표 전부 스킵
    // [위치 기반 배정] "구역 안에서 읽힌 번호는 그 구역 것" — 번호판 oid 의 마지막
    //   중심좌표(정규화)를 매 프레임 기록, FINAL 순간 그 좌표가 든 칸에 배정.
    std::map<long, std::pair<double, double>> plate_pos;
    // [WiseAI 감지 상태] 번호판 객체 수신 여부 전이 로그용
    bool plate_meta_on = false;
    uint64_t last_plate_meta_ms = 0;

    // ---- 판독 파이프라인 ----
    PlateOcrResult last_plate_ocr;        // 마지막 인식 결과 (HTTP /platetext)
    std::string last_final;               // 마지막 확정 번호 (HTTP /platetext)
    std::map<long, StackAcc> stack_acc;   // 스태킹 누적 (kBurstStack 시)
    // [밀린 판독 회수] 후보 시작 직전에 저장돼 deferred 로 스킵된 크롭의 슬롯
    int pending_ocr_slot = -1;
    uint64_t reread_ms = 0;               // 주차중·번호없음일 때 저장크롭 재판독 스로틀
    // [세션 경계] 재판독이 이전 차의 유물 크롭을 파오는 사고 방지
    uint64_t purge_ms = 0;                // 마지막 출차 정리 시각
    uint64_t last_save_ms = 0;            // 마지막 크롭 저장 시각
    // [재도전 라운드] 교착 시 투표함 폐기 + 백지 재수집 스로틀 (08-04)
    uint64_t retry_ms = 0;
    uint64_t hb_ms = 0;                   // [진단] 판독 현황 심장박동 (5초)
    uint64_t read_start_ms = 0;           // 판독 시작(NeedsRead 상승엣지) — 굿샷 우선 창 기준
    bool park_read_prev = false;          // NeedsRead 상승엣지 검출용
    // [베스트 프레임 08-11] 진입~주차완료 동안 버스트 크롭의 sharp×크기 점수를 기록해
    //   챔피언 1장만 보관 → 주차 완료 순간 그 한 장만 OCR (WiseAI 굿샷 폐기, 사용자 설계).
    cv::Mat best_crop;                    // 최고 점수 크롭 (BGR 원본)
    double best_score = 0;                // sharp × sqrt(면적) 점수
    double best_ocr_score = 0;            // 마지막 OCR 한 챔피언 점수 (동일 크롭 재OCR 방지)
    long best_oid = 0;                    // 그 크롭의 번호판 oid
    bool best_finalized = false;          // 이 세션 챔피언 OCR 완료(확정) 여부
    // [FINAL 증거 08-13] 마지막으로 표를 낸 버스트 크롭 — FINAL 순간 "실제 사용된
    //   이미지"로 final_ 링에 저장 (버스트 승리 시 디스크에 크롭이 없어서 보관).
    cv::Mat last_burst_crop;
    long last_burst_oid = 0;
    // [가시화 08-14] 게이트 미달 버스트 판독 로그 스로틀 — "왜 안 읽히나"가 보이게
    uint64_t burst_rej_ms = 0;
    // [헛수고 차단] 이 슬롯의 크롭은 "이미 배정된 번호"로 판명 — 재판독 금지 (08-05)
    int futile_slot = -1;
    // [유령 명부] 출차한 차의 번호판 id → 출차 시각 — 얼어붙은 반복 레코드가
    //   빈칸을 재점화하지 못하게 15초간 점유 증거에서 제외 (새 id 는 즉시 통과).
    std::map<long, uint64_t> ghost_plate;

    // ---- 주차 ----
    bool park_attn = false;               // 구역 진입 상승엣지 로그용
    uint64_t park_attn_log_ms = 0;        // zone entry 로그 스로틀(5초)
    bool park_evt_state = false;          // [EventStatus] 마지막 통지 상태
    // [최후 판독] zone-ocr 스로틀 (08-06 발동부 제거 — 함수 보존용 상태만 잔류)
    uint64_t zfb_ms = 0;
    uint64_t zfb_ver = 0;
    // [출차 육안검증] 부재 의심 칸 스냅샷 확인 (빈칸 N연속 → LEFT)
    uint64_t presence_ms = 0;             // 검사 주기 스로틀
    uint64_t presence_ver = 0;            // 새 프레임에서만 검사 (스냅샷 세대)
    uint64_t presence_hold_ms = 0;        // "주차 유지" 로그 스로틀
    // 번호 미확정 칸의 직전 육안검증 읽기 — 연속성 대조용 (환각은 매번 다른 번호)
    std::map<std::string, std::string> presence_txt;

    // ---- 스냅샷·버스트 ----
    std::string last_jpeg;                // 마지막 "완성된" JPEG (메모리 캐시)
    uint64_t jpeg_ver = 0;                // 스냅샷 세대 — 같은 프레임 재탕 방지
    std::map<long, uint64_t> burst_ver;   // oid 별 마지막으로 판독한 스냅샷 세대
    uint64_t last_snap_trigger = 0;       // 마지막 스냅샷 주문 시각(ms)

    // ---- 계측 (08-07 가속화 — "느림"의 출처를 숫자로) ----
    uint64_t tr_entry_ms = 0;       // 구역 진입(주목 상승엣지) 시각
    uint64_t tr_parked_ms = 0;      // PARKED 확정 시각
    uint64_t tr_first_read_ms = 0;  // 첫 판독 재료(굿샷/버스트 첫 OCR) 시각
    uint64_t tr_final_ms = 0;       // 첫 FINAL — 이때 ⏱ 사이클 요약 1회 발행
    uint64_t frame_warn_ms = 0;     // 프레임 처리시간 경보 스로틀
    uint64_t dec_ms_sum = 0;        // 스냅샷 imdecode 누적 ms (hb 5초 창)
    int dec_n = 0;                  // 그 횟수 — 같은 프레임 중복 디코드 = 캐시 기회
    // [가속 08-07] 디코드 캐시 — 같은 스냅샷 세대는 1회만 imdecode (~100ms/회 실측).
    //   버스트 12연사·육안검증이 같은 프레임을 각자 디코드하던 중복 제거.
    //   메타데이터 스레드 전용 (버스트/육안검증 모두 그 스레드) — 락 불필요.
    cv::Mat dec_frame;
    uint64_t dec_frame_ver = 0;

    // ---- 진단·웹 ----
    int imgref_ch = 0;                    // ImageRef 붙은 번호판 수 (크롭이 어느 채널에 오나)
    std::string last_xml;                 // 마지막 차량 객체 프레임 XML (fallback)
    std::string last_plate_xml;           // 마지막 "번호판 든 객체 프레임" XML (우선)
    std::string raw_events;               // 누적 이벤트 알림(중복제거) → /rawevents
    std::string last_imgref;              // 마지막 번호판 ImageRef 경로
    std::vector<Detection> latest;        // 최신 감지 박스 (HTTP /detections)
    double frame_w = 0;                   // 메타데이터 좌표계 프레임 폭(px)
    double frame_h = 0;                   // 메타데이터 좌표계 프레임 높이(px)
  };
  ChState chs_[cfg::kChannels];
  ChState& C(int ch) { return chs_[ch]; }
  const ChState& C(int ch) const { return chs_[ch]; }

  // 카메라에 스냅샷 1장 주문 + 완성 파일 읽어 last_jpeg 갱신 (라이브뷰용)
  void RefreshSnapshot(int channel);

  // [3단계] 도메인 모듈 — 번호판 저장(파일 I/O, 채널 내부관리)
  PlateStore plate_store_;

  // 번호판 숫자 인식(OCR) — 새 번호판 저장 직후 그 크롭으로 1회 인식.
  PlateOcr plate_ocr_;
  bool plate_ocr_ready_ = false;                 // 모델 로드 성공 여부
  void RecognizePlate(int ch, int slot);         // cap_<slot>.jpg 읽어 인식 + 이벤트+저장

  // [DB 매칭] 등록차량 목록 — HOLD 회수(db-rescue)·FINAL 교정(db-fix)
  PlateDb plate_db_;
  bool plate_db_ready_ = false;

  // [시간축 버스트] 추적 중 bbox+스냅샷으로 샘플 추가 캡처 → 자리별 다수결로 최종 확정
  PlateVote plate_vote_;
  // zone=true: 주차구역 안 번호판 — 웜업 없이 연사(상한 12·300ms 간격)로 존나 읽는다.
  void BurstSample(int ch, long oid, float l, float t, float r, float b, uint64_t now_ms,
                   bool zone = false);
  // [베스트 프레임] 진입~주차 동안 모은 챔피언 크롭 1장을 OCR → FINAL (주차완료 시 호출)
  void FinalizeBestFrame(int ch, uint64_t now_ms);

  // [EV 판정] 최근 ★FINAL 이력 — /isev 가 "그 번호를 실제로 봤는가 + 판 색"을 조회.
  //   color 는 그 채널 마지막 good-shot 의 판 색 분류(wh/ye/gr/bl) — bl = 전기차 파란판.
  struct FinalRec { std::string text; std::string color; uint64_t ms; };
  std::deque<FinalRec> finals_log_;              // 최근 64건 (전 채널 공용)
  void RecordFinal(int ch, const std::string& text, uint64_t now_ms, long oid);

  // [주차 조기개표] 구역에 번호판이 보이는 동안, 추적 종료(stale)를 기다리지 않고
  //   현재 투표함을 심사(Peek) — 신뢰도가 차면 즉시 FINAL, 아니면 계속 수집.
  void TryEarlyFinalize(int ch, uint64_t now_ms);
  // 한 대(oid)만 심사 — 표 추가 직후 즉석 개표용 (plate_seen 에 없어도 동작).
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

  // [EventStatus] 주차위반을 카메라 SUNAPI eventstatus.cgi 네이티브 이벤트로 노출.
  //   번들된 OpenEventDispatcher(.so)가 앱↔펌웨어 EventstatusCGIDispatcher 를 중계.
  void SendParkingEventSchema(Event* event);   // eEventstatusSchema 응답 (JSON+TEXT)
  void SendParkingMetaSchema(Event* event);    // eMetadataSchema 응답 (ONVIF 토픽)
  // [EventStatus 벨] 의미 = "주차 이벤트" — 점유칸 생기면 True, 전부 비면 False.
  //   내용 변화는 펄스(False→True)로 diff 라인 강제. force_state: -1=현재값 / 0/1.
  void NotifyParkingEvent(int ch, bool check, int force_state = -1);
  void CheckParkingEvent(int ch);              // 점유/내용 전이 감지 → 통지·펄스
  void NotifyParkingSpace(int ch, int rule_idx, const ParkingZone::Snap& s, bool check);
  // [벨 2호] ParkingOccupied — 칸별(룰=칸 순번) 점유 불리언 통지
  void NotifyOccupied(int ch, int rule_idx, const std::string& space_id, bool occ, bool check);
  bool CheckParkingStatus(int ch);             // 칸별 전이 감지 (반환: 바뀐 칸 있었나)
  std::map<std::string, ParkingZone::Snap> park_snap_sent_;  // 칸 id → 마지막 통지 상태

  // [진단] 어디서 놓치는지 카운트 (감지 단계 — 저장은 PlateStore 가 셈)
  int plates_seen_ = 0;   // 새로 본 번호판 객체 수
  int imgref_seen_ = 0;   // 그중 ImageRef 붙은 수

  // 감지 대상 on/off (웹 /config 로 조절). 기본 둘 다 켬.
  bool detect_person_ = true;
  bool detect_vehicle_ = true;

  std::string last_plate_;        // 마지막으로 알린 번호판 텍스트 (중복 방지)
  bool plate_raw_dumped_ = false; // 번호판 raw 포맷 확인용 1회 덤프

  void FinalizeStalePlates(int ch, uint64_t now_ms);  // 만료 번호판 확정 (전 채널 대상 호출)
  // [완전 리셋] 출차 순간 그 채널의 판독 진행상태(투표함·추적·스택) 전부 폐기 —
  //   이전 차의 표가 다음 차에 이월되어 옛 번호가 꽂히는 사고 방지 (49허5678 유령).
  void PurgePlateState(int ch);

  // [최후 판독] (08-06 발동부 제거 — 저신뢰 환각 표 오염) 함수는 보존.
  void ZoneFallbackOcr(int ch, uint64_t now_ms);
  // [출차 육안검증] 부재 의심 칸을 스냅샷으로 직접 확인 — 빈칸 N연속일 때만 LEFT.
  //   (WiseAI 는 정지차를 목록에서 빼버려 부재≠출차 — 08-06 가짜 출차 연발 수리)
  void PresenceCheck(int ch, const std::vector<ParkingZone::PendingRect>& rects,
                     uint64_t now_ms);

  std::string app_id_;  // 스냅샷 요청용 앱 ID (eInformAppInfo 로 수신)

  // [1단계] 이벤트가 나가는 출구 목록. 지금은 DebugViewerSink 하나뿐.
  //   나중에 HttpPushSink/RecordSink 등을 push_back 만 하면 확장(도메인 코드 무수정).
  std::vector<IEventSink*> sinks_;

  std::mutex jpeg_mtx_;  // last_jpeg 동시접근 보호(http vs 메타 스레드)
};
