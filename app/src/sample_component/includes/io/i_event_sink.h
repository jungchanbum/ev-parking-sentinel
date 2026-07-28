#pragma once

#include <string>

// ============================================================================
// IEventSink — "이벤트를 받아 소비하는 자"의 계약(interface)
//   앱은 결과를 이 계약에만 넘기고, 실제 목적지(디버그뷰어/서버/녹화 등)는
//   이 계약을 구현한 sink 를 갈아끼워 결정한다. (출구에 뚫어둔 구멍)
// ============================================================================
class IEventSink {
 public:
  virtual ~IEventSink() = default;
  // channel: 어느 채널의 이벤트인지, msg: 사람이 읽을 메시지
  virtual void OnEvent(int channel, const std::string& msg) = 0;
};
