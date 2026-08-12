#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "config.h"
#include "ocr/plate_decode.h"

// ============================================================================
// PlateVote — 차량(oid)별 판독 샘플 누적 → "신뢰도 챔피언십"으로 최종 확정.
//   샘플 출처: good-shot OCR(primary) + 버스트(스냅샷 직접크롭, Q45 미압축).
//   기존 '다수결(good-shot 2표)'은 FINAL≡good-shot이라 무의미했음(실측) →
//   유효포맷 샘플 중 conf 최고를 채택. good-shot이 작은/저화질 크롭일 때
//   고해상 버스트 샘플이 역전할 수 있게 하는 것이 목적(②버스트 승격).
//   조기확정 방지: primary 없이 버스트만일 땐 샘플 2개 이상일 때만 확정.
// ============================================================================
class PlateVote {
 public:
  // 이 oid 에 지금 버스트 샘플을 시도해도 되는가.
  //   일반: 웜업 1초 + 스로틀 + 상한 3개 (지나가는 차 — 비용 절약).
  //   zone=true (주차구역 안 번호판): 웜업 없음·상한 12·스로틀 300ms — 3초 대기 동안
  //   차가 멈춰 제일 잘 보이는 순간이므로 연사로 존나 읽는다.
  bool CanSample(int ch, long oid, uint64_t now_ms, bool zone = false) {
    Entry& e = map_[Key(ch, oid)];
    if (e.first_ms == 0) e.first_ms = now_ms;                     // 첫 감지 시각
    const int max_n = zone ? 12 : cfg::kBurstMax;
    const uint64_t warmup = zone ? 0 : cfg::kBurstWarmupMs;
    const uint64_t throttle = zone ? 300 : cfg::kBurstThrottleMs;
    if (now_ms - e.first_ms < warmup) return false;               // 웜업: 먼 차 스킵
    if ((int)e.samples.size() >= max_n) return false;
    if (e.last_ms != 0 && now_ms - e.last_ms < throttle) return false;
    e.last_ms = now_ms;
    return true;
  }

  void Add(int ch, long oid, const std::string& text, double conf, bool primary) {
    Entry& e = map_[Key(ch, oid)];
    e.samples.push_back({text, conf, primary});
    if (primary) e.has_primary = true;
  }

  int Count(int ch, long oid) const {
    auto it = map_.find(Key(ch, oid));
    return it == map_.end() ? 0 : (int)it->second.samples.size();
  }

  // [가속 08-10] 단일 최고표 조회 — "등록차 정확일치 즉시확정"용. 챔피언십(2표 합의)을
  //   기다리기 전, 유효포맷 중 conf 최고 샘플 하나를 꺼내 호출측이 등록부와 대조한다.
  //   (판정 권한은 호출측 DB 정확일치가 담당 — 여기는 조회만.)
  bool BestSample(int ch, long oid, std::string* text, double* conf) const {
    auto it = map_.find(Key(ch, oid));
    if (it == map_.end()) return false;
    const Sample* best = nullptr;
    for (const auto& s : it->second.samples)
      if (plate_decode::ValidPlateFormat(s.text) && (!best || s.conf > best->conf))
        best = &s;
    if (!best) return false;
    *text = best->text;
    *conf = best->conf;
    return true;
  }

  // 투표함에 text 와 다른 텍스트가 min_conf 이상으로 이미 들어와 있는가.
  //   즉시확정 발사 전 충돌 검사용 — 버스트가 먼저 정답 0.98 을 내놨는데 good-shot
  //   오독 0.99 가 그걸 무시하고 즉시확정한 사고(22소2542) 봉합.
  bool HasConflict(int ch, long oid, const std::string& text, double min_conf) const {
    auto it = map_.find(Key(ch, oid));
    if (it == map_.end()) return false;
    for (const auto& s : it->second.samples)
      if (s.text != text && s.conf >= min_conf) return true;
    return false;
  }

  // good-shot(primary) 샘플이 이미 들어왔는가 — 없으면 확정을 미뤄서
  // 늦게 써지는 good-shot 크롭(RetryPending 최대 3초)을 기다린다.
  bool HasPrimary(int ch, long oid) const {
    auto it = map_.find(Key(ch, oid));
    return it != map_.end() && it->second.has_primary;
  }

  // 추적 종료 시: "일관성 보너스 챔피언십"으로 최종 확정. 항목 제거.
  //   진짜 판독은 샘플마다 같은 텍스트를 반복하고, 환각은 매번 다른 텍스트를 낸다(실측:
  //   정답 25누5701×4 를 오답 28누5781×1(conf 0.99)가 순수 conf 로 이긴 사건).
  //   → 같은 텍스트 그룹의 점수 = max(conf) + 0.02×(반복수-1, 최대 3회분).
  //   *n=샘플수, *conf=승자 점수 근거 conf, *from_primary=승자 그룹에 good-shot 포함 여부.
  //   확정 보류("" 반환): 샘플 없음 / 유효포맷 없음 / 버스트만인데 1개뿐.
  std::string Finalize(int ch, long oid, int* n, double* conf, bool* from_primary) {
    auto it = map_.find(Key(ch, oid));
    if (it == map_.end()) { *n = 0; *conf = 0.0; *from_primary = false; return ""; }
    Entry e = it->second;
    map_.erase(it);
    return Judge(e, n, conf, from_primary);
  }

  // [주차 조기개표] Finalize 와 같은 심사를 투표함을 비우지 않고 수행 — 신뢰도가
  //   아직 부족하면 호출측이 그냥 넘어가고 샘플이 계속 쌓인다 (추적 종료 불필요).
  std::string Peek(int ch, long oid, int* n, double* conf, bool* from_primary) const {
    auto it = map_.find(Key(ch, oid));
    if (it == map_.end()) { *n = 0; *conf = 0.0; *from_primary = false; return ""; }
    return Judge(it->second, n, conf, from_primary);
  }

 private:
  struct Sample { std::string text; double conf; bool primary; };
  struct Entry {
    std::vector<Sample> samples;
    uint64_t first_ms = 0;   // 첫 샘플 시도 시각 ≒ 번호판 첫 감지 (웜업 기준점)
    uint64_t last_ms = 0;
    bool has_primary = false;
  };

  // 챔피언십 본심사 (Finalize/Peek 공용) — 규칙은 기존 Finalize 그대로.
  static std::string Judge(const Entry& e, int* n, double* conf, bool* from_primary) {
    *n = 0; *conf = 0.0; *from_primary = false;
    *n = (int)e.samples.size();
    if (e.samples.empty()) return "";
    if (!e.has_primary && *n < 2) return "";   // 버스트 1개만으론 확정 안 함(조기확정 방지)

    struct Group { int count = 0; double max_conf = 0.0; bool primary = false; };
    std::map<std::string, Group> groups;
    for (const auto& s : e.samples) {
      if (!plate_decode::ValidPlateFormat(s.text)) continue;
      Group& g = groups[s.text];
      g.count++;
      if (s.conf > g.max_conf) g.max_conf = s.conf;
      if (s.primary) g.primary = true;
    }
    if (groups.empty()) return "";

    std::string best_text; double best_score = -1.0; const Group* best_g = nullptr;
    for (const auto& kv : groups) {
      const Group& g = kv.second;
      // 반복 보너스 + good-shot 가중. primary 가중이 없으면 "같은 오독 2회"의 버스트가
      // 정답 good-shot(1.00)을 1.01 로 역전하는 사고 발생(실측 25누5700 사건).
      double score = g.max_conf + 0.02 * std::min(g.count - 1, 3) + (g.primary ? 0.02 : 0.0);
      // 동점이면 max_conf 높은 쪽 우선 (그 다음에야 primary) — primary 우선 동점 규칙이
      // 버스트 정답 1.00 을 good-shot 오답 0.98(+가중 0.02=동점)로 눌러버린 사고 봉합
      // (27어8957 vs 27머8257 실측, 2026-07-28).
      bool better = score > best_score ||
                    (score == best_score && best_g != nullptr &&
                     (g.max_conf > best_g->max_conf ||
                      (g.max_conf == best_g->max_conf && g.primary && !best_g->primary)));
      if (better) {
        best_score = score; best_text = kv.first; best_g = &g;
      }
    }
    *conf = best_g->max_conf; *from_primary = best_g->primary;
    // ---- 한글 접전 감지: 숫자는 완전히 같고 한글만 다른 경쟁 그룹이 conf 0.03 이내로
    // 붙어 있으면 증거 불충분 → 캡(HOLD 행). tinyLPR 약점이 한글 음절 구분이라
    // 오확정 전원이 이 패턴이었음(27하8257/15누1199/22소2542 — 전부 정답이 버스트에
    // 있었는데 근소 차로 눌림). 교차합의(primary+2표 일치) 우승자는 면제.
    // 우승자 max_conf 0.99+ 는 접전 면제 — 0.97 오독 잡음이 1.00 정답을 "접전"으로
    // 눌러 HOLD 시킨 사고 봉합(09서2645 인질 사건). 0.99/1.00 은 무조건 통과.
    bool contested = false;
    if (best_g->max_conf < cfg::kInstantFinalConf &&
        !(best_g->primary && best_g->count >= 2)) {
      for (const auto& kv : groups) {
        if (kv.first == best_text) continue;
        if (DigitsOf(kv.first) == DigitsOf(best_text) &&
            std::fabs(kv.second.max_conf - best_g->max_conf) <= 0.03) {
          contested = true;
          break;
        }
      }
    }
    if (contested) *conf = std::min(*conf, cfg::kRescueConfCap);
    // 게이트용 conf 를 증거 수준에 맞게 조정 (오늘 전 로그 실측 규칙):
    //  - 교차 합의(good-shot+버스트가 같은 텍스트, 2표+)는 틀린 적 0회 → 0.97 로 승격
    //    (36라7833 이 0.93×2 합의인데 HOLD 로 버려지던 문제)
    //  - 버스트 단독 그룹은 정답일 땐 항상 0.98+, 오답은 0.95~0.97 에 분포 →
    //    표 수와 무관하게 conf<0.98 이면 0.95 로 눌러 HOLD (반복 환각 28누5701 차단)
    if (best_g->primary && best_g->count >= 2)
      *conf = std::max(*conf, 0.97);
    else if (!best_g->primary && best_g->max_conf < 0.98)
      *conf = std::min(*conf, cfg::kRescueConfCap);
    return best_text;
  }

  // 텍스트에서 숫자만 추출 — "한글만 다른 접전" 판별용 (27하8257 vs 27머8257 → "278257" 동일)
  static std::string DigitsOf(const std::string& t) {
    std::string d;
    for (char c : t)
      if (c >= '0' && c <= '9') d += c;
    return d;
  }
  static long long Key(int ch, long oid) { return ((long long)ch << 48) ^ (long long)oid; }
  std::map<long long, Entry> map_;
};
