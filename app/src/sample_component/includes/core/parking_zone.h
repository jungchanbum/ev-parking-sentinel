#pragma once

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "config.h"
#include "park_tune.h"

// ============================================================================
// ParkingZone — 주차구역(4점 polygon) 설정·판정·상태를 소유하는 순수 로직 모듈.
//   (SDK/OpenCV 의존 없음 — 좌표 수학 + 파일 IO 만. sample_component 가 얇게 호출.)
//
//   흐름 (Pi 폴링 전용):
//     ① Pi 가 POST /parking/roi 로 4점 보냄 → Add(): ch<N>-<seq> ID 자동부여 +
//        좌우반전(거울상 채널 보정, x→1-x) 저장 + 파일 영속화.
//     ② 매 프레임 Update(): 차 bbox 가 구역 면적 kParkOverlapFrac(75%) 이상 겹치고
//        정지(motion 이 moving=false)한 채 kParkDwellMs(3s) 유지 → occupied + parked_at.
//        점유 차가 빠지면(leave_grace 후) 디폴트 리셋.
//     ③ 번호판 FINAL → AssignAt(): 읽힌 좌표가 든 칸에 번호 기록 + EV 판정 반영.
//        미등록차 EV 실조회 결과는 OnEvResult() 가 나중에 채움.
//     ④ GET /parking/status → StatusXml(): 구역별 <space> 블록 XML (Pi 가 읽어감).
//
//   ev 상태: -1=unknown(판정중/미확정), 0=no(비EV), 1=yes(EV).
//   violation: 주차완료 && 번호판 FINAL && ev==no (unknown 은 pending, 위반 아님).
// ============================================================================
class ParkingZone {
 public:
  struct Pt { double x, y; };

  struct Space {
    std::string id;               // "ch3-01"
    int channel = 0;
    std::vector<Pt> poly;         // 4점, 판정 좌표계(좌우반전 적용 후)

    // 실시간 상태
    bool occupied = false;
    uint64_t parked_at_ms = 0;
    std::string plate;            // 확정 번호 (없으면 "")
    int ev = -1;                  // -1 unknown / 0 no / 1 yes
    std::string ev_source;        // "registered" | "lookup" | ""
    bool violation = false;
    std::string evidence;         // 증거 크롭 URL

    // 내부 추적(직렬화 안 함)
    long occ_oid = 0;             // 현재 점유 차량 oid
    long cand_oid = 0;            // 주차 카운트다운 중인 후보 차량
    uint64_t cand_since_ms = 0;   // 후보가 정지+겹침 시작한 시각
    uint64_t leave_since_ms = 0;  // 점유 차가 안 보이기 시작한 시각(0=보임)
    uint64_t plate_at_ms = 0;     // 번호가 이 칸에서 마지막으로 확인된 시각
    uint64_t evidence_ms = 0;     // 차량/번호판 증거가 마지막으로 보인 시각 (깜빡임 흡수)
    int miss_checks = 0;          // 출차 육안검증(스냅샷) 연속 빈칸 횟수
    double plate_x = -1, plate_y = -1;  // 번호가 마지막으로 읽힌 좌표 — 육안검증 정조준용
                                        //   (구역을 작게 그리면 판이 구역 밖에 걸린다)
  };

  // 매 프레임 넘겨줄 차량 1대 정보 (좌표는 정규화 0~1, 판정 좌표계 기준)
  struct Vehicle { long oid; double l, t, r, b; bool stationary; };
  // 번호판 중심점 — 차량 객체가 안 잡혀도(장기 정지차는 카메라가 목록에서 뺌)
  //   구역 안 번호판을 점유 증거로 쓴다 (08-04 실측: 번호판은 계속 오는데 차량이
  //   안 와서 점유 불가 → 배정번호가 10초마다 "스쳐간" 청소되는 무한루프).
  struct PlatePt { long oid; double x, y; };

  void SetPersistPath(const std::string& p) { path_ = p; Load(); }

  // [런타임 튜닝] 판정 상수의 출처 — 미설정이면 컴파일 기본값으로 동작.
  void SetTune(const ParkTune* t) { tune_ = t; }
  double TOverlap() const { return tune_ ? tune_->overlap : cfg::kParkOverlapFrac; }
  uint64_t TDwell() const { return tune_ ? tune_->dwell_ms : cfg::kParkDwellMs; }
  uint64_t TGrace() const { return tune_ ? tune_->grace_ms : cfg::kParkLeaveGraceMs; }
  double TExitCover() const { return tune_ ? tune_->exit_cover : 0.10; }

  // ---- 설정 API ----------------------------------------------------------

  // 4점 추가. flip=true 면 좌우반전(x→1-x)해서 저장 (Pi 표시좌표용). HTML 오버레이는
  //   판정좌표계에서 직접 찍으므로 flip=false. 반환: 부여된 ID. 실패 시 빈 문자열.
  //   want_id: 클라이언트 지정 ID (08-10 — "수정 = 같은 ID로 삭제→재추가" 지원).
  //   비면 자동부여(chN-MM). 중복 ID 는 실패. chN-MM 꼴이면 자동부여 시퀀스와 동기화.
  std::string Add(int ch, const std::vector<Pt>& pts, bool flip = true,
                  const std::string& want_id = "") {
    if (ch < 0 || ch >= cfg::kChannels || pts.size() != 4) return "";
    Space s;
    s.channel = ch;
    if (!want_id.empty()) {
      for (const auto& sp : spaces_)
        if (sp.channel == ch && sp.id == want_id) return "";  // 중복 — 실패
      s.id = want_id;
      size_t dash = want_id.find('-');
      if (dash != std::string::npos) {
        int mm = atoi(want_id.c_str() + dash + 1);
        if (mm > seq_[ch]) seq_[ch] = mm;   // 이후 자동부여가 이 번호를 안 밟게
      }
    } else {
      char id[24];
      snprintf(id, sizeof(id), "ch%d-%02d", ch, ++seq_[ch]);
      s.id = id;
    }
    for (const auto& p : pts)
      s.poly.push_back({flip ? 1.0 - p.x : p.x, p.y});   // ★ 좌우반전(거울상 채널)
    spaces_.push_back(s);
    Save();
    return s.id;
  }

  bool Remove(int ch, const std::string& id) {
    for (size_t i = 0; i < spaces_.size(); i++)
      if (spaces_[i].channel == ch && spaces_[i].id == id) {
        spaces_.erase(spaces_.begin() + i);
        Save();
        return true;
      }
    return false;
  }

  void Clear(int ch) {
    std::vector<Space> keep;
    for (auto& s : spaces_) if (s.channel != ch) keep.push_back(s);
    spaces_.swap(keep);
    seq_[ch] = 0;
    Save();
  }

  int size() const { return (int)spaces_.size(); }

  // 그 채널 구역에 차가 들어와 있나(정지 무관, 마지막 Update 기준) — 판독 조기가동 트리거.
  bool Attention(int ch) const {
    return ch >= 0 && ch < cfg::kChannels && attn_[ch];
  }

  // [최후 판독용] 번호를 기다리는 점유칸의 (id, 점유차 oid, 폴리곤 bbox 정규화) 목록.
  //   카메라가 번호판 객체를 안 주는 정지차 — 구역 영역을 직접 잘라 OCR 하기 위한 힌트.
  struct PendingRect {
    std::string id; long oid; double l, t, r, b;
    std::string plate;
    double px, py;   // 번호가 마지막으로 읽힌 좌표 (없으면 -1) — 검증 창 정조준용
  };
  std::vector<PendingRect> PlatelessOccupiedRects(int ch) const {
    std::vector<PendingRect> out;
    for (const auto& s : spaces_) {
      if (s.channel != ch || !s.occupied || !s.plate.empty() || s.poly.size() < 3) continue;
      PendingRect pr{s.id, s.occ_oid, 1.0, 1.0, 0.0, 0.0, "", -1.0, -1.0};
      for (const auto& p : s.poly) {
        if (p.x < pr.l) pr.l = p.x;
        if (p.y < pr.t) pr.t = p.y;
        if (p.x > pr.r) pr.r = p.x;
        if (p.y > pr.b) pr.b = p.y;
      }
      out.push_back(pr);
    }
    return out;
  }

  // [출차 육안검증 ①] 부재 유예를 넘긴 점유칸 목록 — 호출측이 스냅샷을 잘라
  //   "칸에 아직 번호판이 보이는가"를 확인한다 (부재만으로는 출차 확정 금지).
  std::vector<PendingRect> AbsenceSuspects(int ch, uint64_t now_ms) const {
    std::vector<PendingRect> out;
    for (const auto& s : spaces_) {
      if (s.channel != ch || !s.occupied || s.leave_since_ms == 0) continue;
      if (now_ms - s.leave_since_ms < TGrace()) continue;
      PendingRect pr{s.id, s.occ_oid, 1.0, 1.0, 0.0, 0.0, s.plate, s.plate_x, s.plate_y};
      for (const auto& p : s.poly) {
        if (p.x < pr.l) pr.l = p.x;
        if (p.y < pr.t) pr.t = p.y;
        if (p.x > pr.r) pr.r = p.x;
        if (p.y > pr.b) pr.b = p.y;
      }
      out.push_back(pr);
    }
    return out;
  }

  // [출차 육안검증 ②] 칸에서 번호판이 아직 읽힘 — 차가 그대로 있다. 의심 해제.
  void PresenceSeen(int ch, const std::string& id, uint64_t now_ms) {
    for (auto& s : spaces_)
      if (s.channel == ch && s.id == id) {
        s.evidence_ms = now_ms; s.leave_since_ms = 0; s.miss_checks = 0;
        return;
      }
  }

  // [출차 육안검증 ③] 검증에서 빈칸 1회 적립. 반환: 누적 연속 빈칸 횟수.
  int PresenceMiss(int ch, const std::string& id) {
    for (auto& s : spaces_)
      if (s.channel == ch && s.id == id) return ++s.miss_checks;
    return 0;
  }

  // [출차 육안검증 ④] 빈칸이 충분히 확인됐다 — 출차 확정(리셋).
  bool ForceLeave(int ch, const std::string& id, uint64_t now_ms) {
    (void)now_ms;
    for (auto& s : spaces_)
      if (s.channel == ch && s.id == id && s.occupied) { ResetLive(s); return true; }
    return false;
  }

  // 그 채널에 "점유됐는데 번호 없는" 구역이 있나 — 최근 FINAL 소급 배정 트리거용.
  //   (일시정지 리그·실주행 모두 FINAL 이 주차확정(3초)보다 먼저 오는 경우가 흔함)
  bool HasPlatelessOccupied(int ch) const {
    for (const auto& s : spaces_)
      if (s.channel == ch && s.occupied && s.plate.empty()) return true;
    return false;
  }

  // [EventStatus] 채널 위반상태 — 위반 구역이 하나라도 있으면 true.
  //   ids:    위반 구역 id 콤마 목록 (SUNAPI 이벤트 RuleName 으로 노출)
  //   plates: 위반 차량 번호 콤마 목록 (ids 와 같은 순서, 이벤트 Data.Plate)
  bool ViolationState(int ch, std::string* ids = nullptr,
                      std::string* plates = nullptr) const {
    bool any = false;
    for (const auto& s : spaces_)
      if (s.channel == ch && s.occupied && s.violation) {
        any = true;
        if (ids)    { if (!ids->empty())    *ids += ",";    *ids += s.id; }
        if (plates) { if (!plates->empty()) *plates += ","; *plates += s.plate; }
      }
    return any;
  }

  // ---- 매 프레임 판정 ----------------------------------------------------

  // 그 채널의 차량 목록으로 각 구역의 occupied/parked_at/출차를 갱신.
  //   log != nullptr 이면 상태전이를 사람이 읽을 문자열로 채운다(디버거뷰어 표시용).
  //   반환값: 이번 호출에서 상태가 바뀐 구역이 있으면 true.
  bool Update(int ch, const std::vector<Vehicle>& vehicles, uint64_t now_ms,
              std::vector<std::string>* log = nullptr,
              const std::vector<PlatePt>* plate_pts = nullptr) {
    bool changed = false;
    // [진입 감지] 정지 여부 무관 — 차가 구역에 kParkOverlapFrac 이상 들어온 순간부터
    //   그 채널 "주목" (호출측이 번호판 판독을 즉시 가동. 주차확정 3초를 안 기다림).
    attn_[ch] = false;
    for (const auto& s : spaces_) {
      if (s.channel != ch) continue;
      for (const auto& v : vehicles)
        if (Cover(v, s.poly) >= TOverlap()) { attn_[ch] = true; break; }
      if (attn_[ch]) break;
    }
    // [판 증거 귀속 08-12] 번호판 중심점 → 소속 칸 1개 배타 결정.
    //   배경: 차량 미감지 차(토이카 등 — WiseAI 가 차로 분류 못 함)는 판이 유일한
    //   점유 증거인데, 기존 InPoly 강짜는 "구역을 판보다 작게 그린" 현실에서 증거를
    //   전멸시켰다 (판 x=0.66 vs 폴리곤 끝 0.648 실측 — 후보·유지 불인정 + 306행
    //   "경계 밖 목격=이탈" 오발까지). 판독 게이트·배정 폴백과 같은 반경(burst_margin)
    //   의 확장 bbox 안이면 증거로 인정하되, 옆 칸 판 도둑질 방지로 "가장 가까운 칸
    //   하나"에만 귀속시킨다 (칸 2개가 붙어 있어도 각자 자기 판만 가진다).
    std::map<long, const Space*> pclaim;
    if (plate_pts) {
      const double fm = tune_ ? tune_->burst_margin : 0.35;
      for (const auto& pp : *plate_pts) {
        const Space* best_s = nullptr;
        double best_d = 1e18;
        for (const auto& s : spaces_) {
          if (s.channel != ch || s.poly.size() < 3) continue;
          double l = 1e9, t = 1e9, r = -1e9, b = -1e9;
          for (const auto& p : s.poly) {
            if (p.x < l) l = p.x;
            if (p.y < t) t = p.y;
            if (p.x > r) r = p.x;
            if (p.y > b) b = p.y;
          }
          double mx = (r - l) * fm, my = (b - t) * fm;
          if (pp.x < l - mx || pp.x > r + mx || pp.y < t - my || pp.y > b + my) continue;
          double cx = (l + r) / 2, cy = (t + b) / 2;
          double d = (pp.x - cx) * (pp.x - cx) + (pp.y - cy) * (pp.y - cy);
          if (d < best_d) { best_d = d; best_s = &s; }
        }
        if (best_s) pclaim[pp.oid] = best_s;
      }
    }
    auto claimed = [&pclaim](long oid, const Space& s) {
      auto it = pclaim.find(oid);
      return it != pclaim.end() && it->second == &s;
    };
    for (auto& s : spaces_) {
      if (s.channel != ch) continue;

      if (!s.occupied) {
        // [청소] 점유 안 됐는데 번호만 배정된 채 10초 방치 — 스쳐간 차. 조용히 비운다.
        if (!s.plate.empty() && s.plate_at_ms != 0 && now_ms - s.plate_at_ms > 10000) {
          if (log) log->push_back("🅿️ " + s.id + " 스쳐간 번호 정리 (" + s.plate + ")");
          ResetLive(s);
        }
        // 겹침 60%+ 이면서 정지한 차 찾기
        long best = 0; double bestov = 0.0;
        for (const auto& v : vehicles)
          if (v.stationary && Cover(v, s.poly) >= TOverlap()) { best = v.oid; bestov = Cover(v, s.poly); break; }
        // 차량 객체가 없어도 구역 안 번호판이면 후보 인정 (장기 정지차 대비).
        //   유령(출차한 차의 반복 레코드)은 호출측이 id 명부로 걸러서 plate_pts 에
        //   아예 안 담는다 — 새 차의 새 id 는 즉시 통과 (시간 차단 폐지, 08-06).
        if (best == 0 && plate_pts)
          for (const auto& pp : *plate_pts)
            if (claimed(pp.oid, s)) { best = pp.oid; bestov = -1.0; break; }
        if (best == 0) {
          // [깜빡임 흡수] 감지가 프레임 단위로 끊겨도 3초까지는 카운트다운 유지
          //   (정지 장면은 번호판 감지가 수 초 간격 — 1.5초로는 취소↔재시작 무한반복
          //    으로 3초 dwell 을 영영 못 채웠다. 08-04 실측 재조정)
          if (s.cand_oid != 0 && now_ms - s.evidence_ms <= 3000) continue;
          if (log && s.cand_oid != 0)
            log->push_back("🅿️ " + s.id + " 후보이탈 — 주차 카운트 취소");
          s.cand_oid = 0; s.cand_since_ms = 0; continue;
        }
        s.evidence_ms = now_ms;
        if (s.cand_oid == 0) {   // 새 후보 — 증거 id 가 차량↔번호판으로 바뀌어도 유지
          s.cand_oid = best; s.cand_since_ms = now_ms;
          if (log) {
            char b[112];
            if (bestov >= 0)
              snprintf(b, sizeof(b), "🅿️ %s 후보차 id%ld (겹침 %.0f%%) — 2초 정지 대기…",
                       s.id.c_str(), best, bestov * 100.0);
            else
              snprintf(b, sizeof(b), "🅿️ %s 후보(번호판 기반) id%ld — 2초 대기…",
                       s.id.c_str(), best);
            log->push_back(b);
          }
        }
        if (now_ms - s.cand_since_ms >= TDwell()) {   // dwell 유지 → 주차 확정
          s.occupied = true;
          s.occ_oid = best;
          s.parked_at_ms = now_ms;
          s.leave_since_ms = 0;
          s.cand_oid = 0;
          changed = true;
          if (log) {
            char b[96]; snprintf(b, sizeof(b), "🅿️ PARKED %s (id%ld) — 번호판 판독 시작", s.id.c_str(), best);
            log->push_back(b);
          }
        }
      } else {
        // [즉시 출차] 점유차가 "보이는데" 구역에서 90% 이상 빠졌으면 그 자리에서 출차
        //   (차량 bbox 의 구역 겹침 ≤10%, 또는 그 차 번호판이 구역 밖에서 목격됨).
        //   단 칸 안에서 3초 넘게 안 보이던 id 가 갑자기 밖에서 나타난 경우는 제외 —
        //   WiseAI 는 정지차를 목록에서 뺐다가 id 를 재사용하므로 "다른 차"일 수 있다
        //   (08-06 실측: 가만히 있는 차가 남의 id 로 즉시출차 오발).
        bool moved_out = false;
        if (now_ms - s.evidence_ms <= 3000) {
          for (const auto& v : vehicles)
            if (v.oid == s.occ_oid && Cover(v, s.poly) <= TExitCover()) { moved_out = true; break; }
          if (!moved_out && plate_pts)
            for (const auto& pp : *plate_pts)
              // 점유차의 판이 "이 칸 귀속이 아닌 곳"에서 목격 — 확장 bbox 를 벗어났거나
              //   옆 칸으로 넘어간 것만 이탈. (구 InPoly: 경계 지터로 즉시출차 오발)
              if (pp.oid == s.occ_oid && !claimed(pp.oid, s)) { moved_out = true; break; }
        }
        if (moved_out) {
          if (log) log->push_back("🅿️ LEFT " + s.id + " — 90% 이탈, 즉시 출차");
          ResetLive(s); changed = true;
          continue;
        }
        // [부재 = 출차 아님] WiseAI 는 정지차를 몇 초 만에 메타데이터에서 빼버린다
        //   (08-06 실측: 가만히 있는 차가 부재 4초 만에 가짜 출차 연발). 그래서 여기선
        //   출차 확정을 절대 안 내리고 "부재 의심"만 표시 — 확정은 호출측이 스냅샷으로
        //   칸을 직접 들여다본 육안검증(PresenceMiss 누적 → 빈칸 확인)으로만 내린다.
        //   [08-06 실측 정정] WiseAI 는 크고 선명한 정지차는 같은 id·좌표로 계속
        //   보내준다 (조건부 드랍만 존재). 그래서 점유 지속 증거는 id 무관 — 칸을
        //   절반 이상 덮는 "정지" 차량이 있으면 그게 곧 점유차다 (id 뺑뺑이 흡수).
        //   정지 조건 필수 — 지나가는 차가 칸을 스쳐 덮으면 출차 카운트가 리셋되어
        //   빈칸 확정이 한없이 밀린다 (08-06 실측: 2/3→1/3 되감김).
        bool still = false;
        for (const auto& v : vehicles)
          if (v.stationary && Cover(v, s.poly) >= 0.5) { still = true; s.occ_oid = v.oid; break; }
        if (!still && plate_pts)
          for (const auto& pp : *plate_pts)
            if (claimed(pp.oid, s)) { still = true; break; }
        if (still) { s.evidence_ms = now_ms; s.miss_checks = 0; }
        // [08-12] 깜빡임 흡수창을 grace 와 연동(상한 2s) — 출차→빈칸 체감을 빠르게.
        //   grace 를 낮춰 빠른 출차를 원할 때 고정 2s 가 바닥이 되지 않게 한다.
        uint64_t hold = TGrace() < 2000 ? TGrace() : 2000;
        if (still || now_ms - s.evidence_ms <= hold) s.leave_since_ms = 0;
        else if (s.leave_since_ms == 0) s.leave_since_ms = now_ms;
      }
    }
    return changed;
  }

  // ---- 번호판·EV 연결 ----------------------------------------------------

  // [위치 배정] "구역 안에서 읽힌 번호는 그 구역 것" — 번호판 중심좌표(x,y 정규화)가
  //   들어있는 칸에 배정. oid·명찰·소급 같은 간접 연결 전부 불필요 (위치가 곧 매칭).
  //   같은 번호 재확정이면 갱신시각만 새로 찍고 조용히 통과, 다른 번호면 교체(위치가 진실).
  //   점유 전이어도 배정해둔다 — 위반 통지는 ViolationState 가 occupied 를 함께 보므로
  //   주차확정(3초) 순간 자동으로 발화. 반환: 상태 설명 (변화 없으면 "").
  //   replace_ok: 이미 번호 있는 칸의 "교체" 허용 여부 — DB 등록차 판독만 true.
  //   (08-06 실측: 늦게 온 저품질 굿샷의 미등록 오독 "03저3449"가 위치 일치라는
  //    이유로 정답 09서2645 를 덮음 → 미등록 판독은 빈 칸만 채우고 교체는 금지)
  std::string AssignAt(int ch, double x, double y, const std::string& plate, int ev_state,
                       const std::string& source, const std::string& evidence, uint64_t now_ms,
                       bool replace_ok = true) {
    // 1차: 좌표가 든 칸 — 위치가 확인된 판독. 이때만 기존 번호 교체 허용
    //   (추적 id 바꿔치기 자가교정용). 폴백: 좌표 판정 실패 시 "번호 없는 진행중 칸"만
    //   채운다 — 절대 기존 번호를 덮지 않는다 (08-04 실측: 구역 밖에서 읽힌 옆 차
    //   번호가 폴백 2단으로 ch0-01 의 정답을 덮은 사고 → 폴백 2단 폐지).
    Space* hit = nullptr;
    bool positional = false;
    for (auto& s : spaces_)
      if (s.channel == ch && InPoly(x, y, s.poly)) { hit = &s; positional = true; break; }
    // 폴백: 좌표가 칸 안은 아니지만 "칸 bbox 20% 확장 범위 안"인 판독만, 번호 없는
    //   진행중 칸에 허용 — 구역을 판보다 작게 그린 경우의 가장자리 판독 구제.
    //   범위 밖 판독은 폐기: 채널 반대편(모니터 속 영상 등)에서 읽힌 남의 번호가
    //   굶고 있는 빈 칸에 꽂히는 사고 방지 (08-06 실측: 36라7833 오배정).
    if (!hit)
      for (auto& s : spaces_) {
        if (s.channel != ch || !(s.occupied || s.cand_oid != 0) || !s.plate.empty()) continue;
        double l = 1e9, t = 1e9, r = -1e9, b = -1e9;
        for (const auto& p : s.poly) {
          if (p.x < l) l = p.x;
          if (p.y < t) t = p.y;
          if (p.x > r) r = p.x;
          if (p.y > b) b = p.y;
        }
        // [08-12] 폴백 반경 = 판독 게이트(burst_margin, 기본 0.35)와 일치.
        //   기존 0.20 은 "읽으라고 해놓고(35%) 배정은 거부(20%)"하는 모순 구간을
        //   만들었다 — 판이 칸 bbox+20~35% 에 걸치면 FINAL 이 무한 재발급되며
        //   영영 안 꽂히는 루프 실측 (342버6986, 판 x=0.66 vs 한계 0.6606).
        //   오배정 방역(모니터 건너편 번호)은 이 반경 밖 폐기가 그대로 담당한다.
        double fm = tune_ ? tune_->burst_margin : 0.35;
        double mx = (r - l) * fm, my = (b - t) * fm;
        if (x >= l - mx && x <= r + mx && y >= t - my && y <= b + my) { hit = &s; break; }
      }
    if (!hit) return "";
    Space& s = *hit;
    if (x > 0.0 || y > 0.0) { s.plate_x = x; s.plate_y = y; }  // 육안검증 정조준 좌표
    if (s.plate == plate) {                       // 같은 번호 재확정 — 갱신만
      s.plate_at_ms = now_ms;
      if (ev_state != -1 && s.ev != ev_state) { ApplyEv(s, ev_state, source); return Describe(s); }
      return "";
    }
    if (!s.plate.empty() && (!positional || !replace_ok)) return "";  // 교체는 위치확인+신뢰 판독만
    s.plate = plate;                              // 새 번호 (빈칸, 또는 위치확인 교체)
    s.evidence = evidence;
    s.plate_at_ms = now_ms;
    ApplyEv(s, ev_state, source);
    return Describe(s);
  }

  // [EventStatus 벨] 채널 점유 종합 — "주차 이벤트 있음" 신호. 점유칸이 하나라도
  //   있으면 true. ids/plates: 점유칸 id·번호 콤마 목록 (판정 전 번호는 "?").
  //   위반 판단은 벨이 아니라 XML(violation 필드)이 담당한다 (08-04 설계 변경).
  bool AnyOccupied(int ch, std::string* ids = nullptr, std::string* plates = nullptr) const {
    bool any = false;
    for (const auto& s : spaces_) {
      if (s.channel != ch || !s.occupied) continue;
      any = true;
      if (ids)    { if (!ids->empty())    *ids += ",";    *ids += s.id; }
      if (plates) { if (!plates->empty()) *plates += ","; *plates += s.plate.empty() ? "?" : s.plate; }
    }
    return any;
  }

  // [EventStatus] 칸별 상태 스냅샷 — 하위 룰(RuleIndex 2..) 통지용.
  struct Snap {
    std::string id;
    bool occupied = false;
    std::string plate;
    int ev = -1;
    bool violation = false;
    bool operator==(const Snap& o) const {
      return occupied == o.occupied && plate == o.plate && ev == o.ev && violation == o.violation;
    }
  };
  std::vector<Snap> Snapshot(int ch) const {
    std::vector<Snap> out;
    for (const auto& s : spaces_)
      if (s.channel == ch)
        out.push_back({s.id, s.occupied, s.plate, s.ev, s.violation && s.occupied});
    return out;
  }

  // 이 채널 어느 칸에 그 번호가 이미 붙어 있나 — 재확정 스팸 방지 (조용히 갱신만).
  bool HasPlate(int ch, const std::string& plate) const {
    for (const auto& s : spaces_)
      if (s.channel == ch && s.plate == plate) return true;
    return false;
  }

  // 그 좌표의 칸이 "완결"(점유+번호+EV 확정) 상태인가 — 완결 칸은 차가 나갈 때까지
  //   판독 완전 침묵 (재확인 없음. 출차는 증거 부재 5초 → LEFT 가 잡는다).
  bool SettledAt(int ch, double x, double y) const {
    for (const auto& s : spaces_)
      if (s.channel == ch && InPoly(x, y, s.poly))
        return s.occupied && !s.plate.empty() && s.ev != -1;
    return false;
  }

  // [판독 필요] 이 채널에 "읽을 일"이 있나 — 후보 카운트다운 중이거나, 점유됐는데
  //   번호/EV 미확정인 칸이 있을 때만. 전 칸 완결이면 false → 채널 판독 완전 침묵
  //   ("일 다 끝난 주차공간에 자꾸 지랄" 방지 — 08-04).
  bool NeedsRead(int ch) const {
    for (const auto& s : spaces_) {
      if (s.channel != ch) continue;
      if (s.cand_oid != 0) return true;
      if (s.occupied && (s.plate.empty() || s.ev == -1)) return true;
    }
    return false;
  }

  // 그 좌표(정규화)가 이 채널 어느 칸 안인가 — 번호판 조기개표 트리거용.
  bool PointInAnyZone(int ch, double x, double y) const {
    for (const auto& s : spaces_)
      if (s.channel == ch && InPoly(x, y, s.poly)) return true;
    return false;
  }

  // 칸 bbox 확장(margin 비율) 범위 안인가 — 판독 가동·배정 폴백의 공통 근접 기준.
  //   구역에서 먼 판(모니터 속 영상, 옆 차선)은 판독 자체를 안 건다.
  //   배정 폴백은 0.20(엄격), 버스트 가동은 0.35(구역을 작게 그린 경우 흡수).
  bool NearAnyZone(int ch, double x, double y, double margin = 0.20) const {
    for (const auto& s : spaces_) {
      if (s.channel != ch || s.poly.size() < 3) continue;
      double l = 1e9, t = 1e9, r = -1e9, b = -1e9;
      for (const auto& p : s.poly) {
        if (p.x < l) l = p.x;
        if (p.y < t) t = p.y;
        if (p.x > r) r = p.x;
        if (p.y > b) b = p.y;
      }
      double mx = (r - l) * margin, my = (b - t) * margin;
      if (x >= l - mx && x <= r + mx && y >= t - my && y <= b + my) return true;
    }
    return false;
  }

  // 이 채널에 구역이 하나라도 있나 — 있으면 판독을 구역 안으로 제한(초점이 구역에 고정).
  bool HasZones(int ch) const {
    for (const auto& s : spaces_)
      if (s.channel == ch) return true;
    return false;
  }

  // [판독 게이트] 이 채널에 "정지한 주차 후보/점유차"가 있나 — 후보 3초 카운트다운
  //   시작(=차가 구역에서 정지) 또는 점유 중일 때만 true. 진입 중(움직임)은 제외 —
  //   모션블러 샘플이 버스트 탄창(12발)을 쓰레기 표로 다 써버리는 사고 방지 (08-04 실측:
  //   진입 중 12발 전부 오독, 정작 정지 후엔 탄창 소진으로 판독 0회).
  bool Busy(int ch) const {
    if (ch < 0 || ch >= cfg::kChannels) return false;
    for (const auto& s : spaces_)
      if (s.channel == ch && (s.occupied || s.cand_oid != 0)) return true;
    return false;
  }

  // 미등록차 EV 실조회 결과 도착. 그 번호를 가진 구역들 갱신.
  //   반환: 갱신된 구역별 설명 문자열 목록 (디버거뷰어용).
  std::vector<std::string> OnEvResult(const std::string& plate, int ev_state) {
    std::vector<std::string> out;
    for (auto& s : spaces_)
      if (!s.plate.empty() && s.plate == plate && s.ev != ev_state) {
        // occupied 조건 없음 — 점유확정(3초) 전에 조회가 끝나도 미리 반영해 둔다.
        ApplyEv(s, ev_state, "lookup");
        out.push_back(Describe(s));
      }
    return out;
  }

  // ---- 출력 --------------------------------------------------------------

  // GET /parking/status : 구역별 실시간 상태 XML (Pi 가 읽어감)
  std::string StatusXml(uint64_t now_ms) const {
    std::ostringstream o;
    o << "<parking>\n";
    for (const auto& s : spaces_) {
      o << "  <space id=\"" << s.id << "\" channel=\"" << s.channel << "\">"
        << "<occupied>" << (s.occupied ? "true" : "false") << "</occupied>"
        << "<parked_ms_ago>" << (s.occupied ? (long)(now_ms - s.parked_at_ms) : -1) << "</parked_ms_ago>"
        << "<plate>" << s.plate << "</plate>"
        << "<ev source=\"" << s.ev_source << "\">" << EvStr(s.ev) << "</ev>"
        << "<violation>" << (s.violation ? "true" : "false") << "</violation>"
        << "<evidence>" << s.evidence << "</evidence>"
        << "<polygon>";      // ★ HTML 오버레이용 — 저장(판정)좌표계 그대로
      for (const auto& p : s.poly) o << "<pt x=\"" << p.x << "\" y=\"" << p.y << "\"/>";
      o << "</polygon>"
        << "</space>\n";
    }
    o << "</parking>\n";
    return o.str();
  }

  // GET /parking/roi : 그 채널의 등록 구역(polygon) 목록 XML — Pi 가 판정좌표계로 되돌려
  //   보려면 x→1-x 재반전. 여기선 저장된(반전된) 좌표 그대로 노출 + 반전여부 표기.
  std::string RoiXml(int ch) const {
    std::ostringstream o;
    o << "<roi channel=\"" << ch << "\" flipped=\"true\">\n";
    for (const auto& s : spaces_) {
      if (s.channel != ch) continue;
      o << "  <space id=\"" << s.id << "\"><polygon>";
      for (const auto& p : s.poly) o << "<pt x=\"" << p.x << "\" y=\"" << p.y << "\"/>";
      o << "</polygon></space>\n";
    }
    o << "</roi>\n";
    return o.str();
  }

 private:
  static const char* EvStr(int e) { return e == 1 ? "yes" : (e == 0 ? "no" : "unknown"); }

  // 구역의 현재 번호/EV/위반 상태를 디버거뷰어용 한 줄로.
  static std::string Describe(const Space& s) {
    std::string r = "🅿️ " + s.id + " plate=" + (s.plate.empty() ? "?" : s.plate);
    if (s.ev == 1)      r += " ev=yes ✅EV-ok";
    else if (s.ev == 0) r += " ev=no ⛔VIOLATION";
    else                r += " ev=unknown ⏳판정대기";
    if (!s.ev_source.empty()) r += " (" + s.ev_source + ")";
    return r;
  }

  static void ResetLive(Space& s) {
    s.occupied = false; s.parked_at_ms = 0; s.plate.clear();
    s.ev = -1; s.ev_source.clear(); s.violation = false; s.evidence.clear();
    s.occ_oid = 0; s.cand_oid = 0; s.cand_since_ms = 0; s.leave_since_ms = 0;
    s.plate_at_ms = 0; s.evidence_ms = 0; s.miss_checks = 0;
    s.plate_x = -1; s.plate_y = -1;
  }

  // ev 반영 + 위반 재계산 (EV전용면 && 번호확정 && ev==no → violation).
  static void ApplyEv(Space& s, int ev_state, const std::string& source) {
    s.ev = ev_state;
    s.ev_source = source;
    s.violation = (!s.plate.empty() && s.ev == 0);   // v1: 모든 구역이 EV전용
  }

  // 점이 다각형 안인가 (ray casting)
  static bool InPoly(double x, double y, const std::vector<Pt>& poly) {
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
      if (((poly[i].y > y) != (poly[j].y > y)) &&
          (x < (poly[j].x - poly[i].x) * (y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x))
        in = !in;
    }
    return in;
  }

  // 구역 면적 중 차량 bbox 가 덮는 비율 — 12x12 격자 표본. 구역을 차보다 작게
  //   그린 경우(차 높이 때문에 bbox 가 큼) Overlap 은 낮아도 이 값은 커진다.
  //   판정은 max(Overlap, ZoneCoverage) — 어느 방향이든 60% 채우면 주차 (08-04).
  static double ZoneCoverage(const Vehicle& v, const std::vector<Pt>& poly) {
    if (poly.size() < 3 || v.r <= v.l || v.b <= v.t) return 0.0;
    double l = 1e9, t = 1e9, r = -1e9, b = -1e9;
    for (const auto& p : poly) {
      if (p.x < l) l = p.x;
      if (p.y < t) t = p.y;
      if (p.x > r) r = p.x;
      if (p.y > b) b = p.y;
    }
    const int N = 12;
    int in_zone = 0, covered = 0;
    for (int iy = 0; iy < N; iy++)
      for (int ix = 0; ix < N; ix++) {
        double x = l + (r - l) * (ix + 0.5) / N;
        double y = t + (b - t) * (iy + 0.5) / N;
        if (!InPoly(x, y, poly)) continue;
        in_zone++;
        if (x >= v.l && x <= v.r && y >= v.t && y <= v.b) covered++;
      }
    return in_zone ? (double)covered / in_zone : 0.0;
  }

  // 겹침 판정 통합 — 양방향 중 큰 쪽 (칸/차 크기 비대칭 흡수)
  static double Cover(const Vehicle& v, const std::vector<Pt>& poly) {
    double a = Overlap(v, poly), b = ZoneCoverage(v, poly);
    return a > b ? a : b;
  }

  // bbox 면적 중 다각형 안에 든 비율 — 격자 6x6 표본으로 근사.
  static double Overlap(const Vehicle& v, const std::vector<Pt>& poly) {
    if (poly.size() < 3 || v.r <= v.l || v.b <= v.t) return 0.0;
    const int N = 6;
    int in = 0, tot = 0;
    for (int iy = 0; iy < N; iy++)
      for (int ix = 0; ix < N; ix++) {
        double x = v.l + (v.r - v.l) * (ix + 0.5) / N;
        double y = v.t + (v.b - v.t) * (iy + 0.5) / N;
        tot++;
        if (InPoly(x, y, poly)) in++;
      }
    return tot ? (double)in / tot : 0.0;
  }

  // ---- 영속화 (재부팅 후 유지; 잃어도 Pi 가 재전송 가능) ----
  //   포맷: 한 줄 "id ch x0 y0 x1 y1 x2 y2 x3 y3" (좌표는 저장된 반전 후 값).
  void Save() const {
    if (path_.empty()) return;
    std::ofstream f(path_.c_str());
    if (!f) return;
    for (const auto& s : spaces_) {
      f << s.id << " " << s.channel;
      for (const auto& p : s.poly) f << " " << p.x << " " << p.y;
      f << "\n";
    }
  }

  void Load() {
    spaces_.clear();
    for (int i = 0; i < 4; i++) seq_[i] = 0;
    std::ifstream f(path_.c_str());
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
      std::istringstream ss(line);
      Space s;
      if (!(ss >> s.id >> s.channel)) continue;
      double x, y;
      while (ss >> x >> y) s.poly.push_back({x, y});
      if (s.poly.size() != 4 || s.channel < 0 || s.channel >= cfg::kChannels) continue;
      spaces_.push_back(s);
      // seq 복원: "chN-MM" 의 MM 최댓값
      int mm = 0;
      size_t dash = s.id.find('-');
      if (dash != std::string::npos) mm = atoi(s.id.c_str() + dash + 1);
      if (mm > seq_[s.channel]) seq_[s.channel] = mm;
    }
  }

  std::vector<Space> spaces_;
  const ParkTune* tune_ = nullptr;  // 런타임 튜닝 (SetTune — 미설정이면 컴파일 기본값)
  int seq_[4] = {0, 0, 0, 0};
  bool attn_[4] = {false, false, false, false};  // 채널별 "구역에 차 진입" (매 Update 갱신)
  std::string path_;
};
