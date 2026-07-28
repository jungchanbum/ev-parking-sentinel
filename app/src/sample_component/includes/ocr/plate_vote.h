#pragma once

#include <algorithm>
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
  //   웜업 1초 + 스로틀 + 상한 6개 (cfg::kBurstWarmupMs/ThrottleMs/Max).
  //   초반 1초는 차가 제일 멀어 최저화질 크롭만 나옴 — 표 가치 없이 CPU 만 먹어서
  //   건너뛴다. 화질 열화 시 버스트가 good-shot(Q45)을 역전하는 보험 역할은 유지.
  bool CanSample(int ch, long oid, uint64_t now_ms) {
    Entry& e = map_[Key(ch, oid)];
    if (e.first_ms == 0) e.first_ms = now_ms;                       // 첫 감지 시각 기록
    if (now_ms - e.first_ms < cfg::kBurstWarmupMs) return false;    // 웜업: 아직 안 딴다
    if ((int)e.samples.size() >= cfg::kBurstMax) return false;
    if (e.last_ms != 0 && now_ms - e.last_ms < cfg::kBurstThrottleMs) return false;
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
    *n = 0; *conf = 0.0; *from_primary = false;
    auto it = map_.find(Key(ch, oid));
    if (it == map_.end()) return "";
    Entry e = it->second;
    map_.erase(it);
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
      if (score > best_score || (score == best_score && g.primary)) {
        best_score = score; best_text = kv.first; best_g = &g;
      }
    }
    *conf = best_g->max_conf; *from_primary = best_g->primary;
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

 private:
  struct Sample { std::string text; double conf; bool primary; };
  struct Entry {
    std::vector<Sample> samples;
    uint64_t first_ms = 0;   // 첫 샘플 시도 시각 ≒ 번호판 첫 감지 (웜업 기준점)
    uint64_t last_ms = 0;
    bool has_primary = false;
  };
  static long long Key(int ch, long oid) { return ((long long)ch << 48) ^ (long long)oid; }
  std::map<long long, Entry> map_;
};
