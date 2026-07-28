#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <utility>
#include <vector>

#include "config.h"

// ============================================================================
// MotionTracker — 한 채널의 객체 움직임 추적.
//   객체(id)별 최근 프레임 중심좌표 궤적으로 "움직이는지"를 판정한다.
//   SDK/이벤트 무관: 판정 결과만 돌려주고, 이벤트 발행은 호출자가 한다.
// ============================================================================
class MotionTracker {
 public:
  // 이번 프레임의 추적 대상(사람/차량, 유효 중심좌표)
  struct Sample { long id; float cx, cy; bool is_vehicle; };
  // 판정 결과 (입력과 같은 순서)
  struct Result {
    bool moving;        // 현재 움직이는 중인가
    bool just_started;  // 이번 프레임에 "정지→움직임" 전환 (이벤트 1회용)
    bool is_vehicle;
  };

  // objs: 이번 프레임 추적 대상. tick: 프레임 카운터. now_ms: 현재 시각(3초 홀드).
  std::vector<Result> Update(uint64_t tick, uint64_t now_ms, const std::vector<Sample>& objs) {
    // 좌표 스케일 자동 감지 (정규화 안 된 좌표계 대응)
    for (const auto& s : objs) {
      double m = std::max(std::fabs((double)s.cx), std::fabs((double)s.cy));
      if (m > coord_scale_) coord_scale_ = m;
    }

    std::vector<Result> out;
    out.reserve(objs.size());
    for (const auto& s : objs) {
      ObjectTrack& tr = tracks_[s.id];
      tr.centers.push_back({s.cx, s.cy});
      while ((int)tr.centers.size() > cfg::kHistoryFrames) tr.centers.pop_front();
      tr.last_seen = tick;

      bool just = false;
      if ((int)tr.centers.size() >= cfg::kCompareBack + 1) {
        const auto& cur = tr.centers.back();
        const auto& past = tr.centers[tr.centers.size() - 1 - cfg::kCompareBack];
        double dx = cur.first - past.first, dy = cur.second - past.second;
        double dist = std::sqrt(dx * dx + dy * dy);
        if (dist > coord_scale_ * cfg::kMoveRatio) {
          tr.moving_until = now_ms + cfg::kMovingHoldMs;  // 계속 움직이면 계속 연장
          if (!tr.moving) { tr.moving = true; just = true; }  // 전환 순간만 표시
        } else if (tr.moving && now_ms >= tr.moving_until) {
          tr.moving = false;  // 3초 무움직임 → 해제
        }
      }
      out.push_back({tr.moving, just, s.is_vehicle});
    }
    return out;
  }

  // 특정 객체가 지금 움직이는 중인지 (번호판 부모 차량 조회용)
  bool IsMoving(long id) const {
    auto it = tracks_.find(id);
    return it != tracks_.end() && it->second.moving;
  }
  // 그 객체가 추적 목록에 있는지 (없으면 번호판 박스는 무조건 표시)
  bool IsTracked(long id) const { return tracks_.find(id) != tracks_.end(); }

  // 오래 안 보인 객체 추적 삭제
  void PruneStale(uint64_t tick) {
    for (auto it = tracks_.begin(); it != tracks_.end();) {
      if (it->second.last_seen + cfg::kStaleFrames < tick) it = tracks_.erase(it);
      else ++it;
    }
  }

 private:
  struct ObjectTrack {
    std::deque<std::pair<float, float>> centers;  // 최근 N프레임 중심좌표
    uint64_t last_seen = 0;
    bool moving = false;
    uint64_t moving_until = 0;  // 이 시각(ms)까지 무조건 moving
  };
  std::map<long, ObjectTrack> tracks_;
  double coord_scale_ = 1.0;  // 좌표 스케일 자동 감지값
};
