#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <utility>

#include "io/i_event_sink.h"

// ============================================================================
// DebugViewerSink — 이벤트를 stdout(로그 CGI) + DEBUG VIEWER 로 내보내는 sink.
//   기존 EmitEvent 의 동작을 그대로 옮겨온 것(동작 불변).
//   SDK 전송(SendTargetEvents)은 소유주(SampleComponent)만 호출할 수 있어
//   생성 시 주입받은 함수 send_to_viewer 로 위임한다.
// ============================================================================
class DebugViewerSink : public IEventSink {
 public:
  explicit DebugViewerSink(std::function<void(const std::string&)> send_to_viewer)
      : send_(std::move(send_to_viewer)) {}

  void OnEvent(int channel, const std::string& msg) override {
    printf("[EVENT ch%d] %s\n", channel, msg.c_str());
    if (send_) send_(msg);
  }

 private:
  std::function<void(const std::string&)> send_;  // DEBUG VIEWER 로 보내는 실제 SDK 호출
};
