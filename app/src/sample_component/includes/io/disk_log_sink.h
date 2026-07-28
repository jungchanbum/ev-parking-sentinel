#pragma once

#include <fstream>
#include <string>

#include "config.h"
#include "io/i_event_sink.h"

// ============================================================================
// DiskLogSink — 모든 이벤트를 파일(../storage/events.log)에 누적 기록하는 sink.
//   감지·저장 이력(감사용). DebugViewerSink 와 별개의 "다른 목적지" 예시 —
//   [4단계] 확장성 증명: 이 sink 는 도메인(MotionTracker/PlateStore/ProcessObjects)
//   코드를 한 줄도 안 건드리고, Initialize 에서 sinks_.push_back 한 줄로 끼운다.
// ============================================================================
class DiskLogSink : public IEventSink {
 public:
  void OnEvent(int channel, const std::string& msg) override {
    std::string path = std::string(cfg::kStorageDir) + "/events.log";
    // 너무 커지면(>64KB) 새로 시작(간단 순환). 이벤트는 드물어 부담 없음.
    std::ifstream chk(path.c_str(), std::ios::binary | std::ios::ate);
    bool too_big = chk && (long)chk.tellg() > 64 * 1024;
    chk.close();
    std::ofstream ofs(path.c_str(), too_big ? std::ios::trunc : std::ios::app);
    if (ofs) ofs << "ch" << channel << " " << msg << "\n";
  }
};
