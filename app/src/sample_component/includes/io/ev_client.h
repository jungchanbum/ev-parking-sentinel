#pragma once

#include <csignal>
#include <cstdio>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "config.h"   // kEvLookupUrl (조회 엔드포인트 — 하드코딩 금지, 단일 출처)

// ============================================================================
// EvClient — 무공해차 통합누리집(ev.or.kr) '내차 저공해 확인' 실조회 클라이언트.
//   미등록 번호판의 전기차 여부를 카메라가 직접 조회한다 (2026-07-30 실측 확정):
//     POST /nportal/buySupprt/selectNonpolluCheck.ajax
//     selectCarNum=1&searchWord=<번호판>  →  {"data":null | {..., "CAR_TYPE_NM":"1종"}}
//     판정: 1종(전기·수소) → EV. 2·3종/목록없음 → 아님. 로그인·캡차·키 불필요.
//
//   [중요] 조회는 카메라 내장 curl/wget 을 "자식 프로세스"로 실행한다 (popen).
//   앱 프로세스 안에서 OpenSSL 로 직접 TLS 를 열면 앱이 즉사한다 — 플랫폼이
//   자체 TLS(PSessionTls)를 쓰는 프로세스에 다른 OpenSSL 을 동거시키는 구조라
//   첫 SSL 호출에서 결정적으로 죽는 것을 실측 (07-30, 12:35/12:49 빌드 2회).
//   자식 프로세스는 TLS 를 자기 주소공간에서 하므로 충돌 자체가 불가능하다.
//
//   조회는 전용 워커 스레드에서 수행 — 메타데이터 파이프라인을 절대 막지 않는다.
//   결과는 콜백으로 돌려주고, 호출측은 큐에 담아 자기 스레드에서 회수한다
//   (EmitEvent 단일 스레드 규율 유지).
// ============================================================================
class EvClient {
 public:
  // verdict: 1=전기·수소(1종), 0=아님(목록없음·2·3종), -1=조회 실패(통신/파싱)
  struct Result {
    int ch;
    std::string plate;
    int verdict;
    std::string detail;  // 성공: "제작사 차명, 연료, N종" / 실패: 단계 설명
  };
  using Callback = std::function<void(const Result&)>;

  ~EvClient() { Stop(); }

  void Start(Callback cb) {
    signal(SIGPIPE, SIG_IGN);  // 파이프/소켓 쓰기 실패가 프로세스를 죽이지 않게
    cb_ = std::move(cb);
    run_ = true;
    th_ = std::thread([this] { Loop(); });
  }

  void Stop() {
    {
      std::lock_guard<std::mutex> lk(mtx_);
      if (!run_) return;
      run_ = false;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
  }

  void Request(int ch, const std::string& plate) {
    {
      std::lock_guard<std::mutex> lk(mtx_);
      q_.push_back({ch, plate});
    }
    cv_.notify_one();
  }

 private:
  struct Job { int ch; std::string plate; };

  void Loop() {
    while (true) {
      Job j;
      {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [this] { return !run_ || !q_.empty(); });
        if (!run_ && q_.empty()) return;
        if (q_.empty()) continue;
        j = q_.front();
        q_.pop_front();
      }
      Result r{j.ch, j.plate, -1, ""};
      try {
        r.verdict = Query(j.plate, &r.detail);
      } catch (const std::exception& e) {
        r.verdict = -1;
        r.detail = std::string("exception: ") + e.what();
      } catch (...) {
        r.verdict = -1;
        r.detail = "unknown exception";
      }
      if (cb_) cb_(r);
    }
  }

  // 예약 문자 제외 전부 %XX (UTF-8 한글 포함 바이트 단위).
  //   결과가 [A-Za-z0-9%._~-] 뿐이라 쉘 명령에 넣어도 안전하다 (인젝션 불가).
  static std::string PctEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
        out += (char)c;
      } else {
        out += '%';
        out += hex[c >> 4];
        out += hex[c & 15];
      }
    }
    return out;
  }

  // body 에서 "key":"값" 의 값 추출 (이 API 필드에 이스케이프 문자 없음 — 단순 스캔)
  static std::string JsonStr(const std::string& body, const char* key) {
    std::string pat = std::string("\"") + key + "\":\"";
    size_t p = body.find(pat);
    if (p == std::string::npos) return "";
    p += pat.size();
    size_t e = body.find('"', p);
    return e == std::string::npos ? "" : body.substr(p, e - p);
  }

  // 명령 실행 → stdout+stderr 수거. 반환: 실행 자체 성공 여부 (popen/sh 레벨).
  static bool Exec(const std::string& cmd, std::string* out) {
    out->clear();
    printf("[EV] exec: %s\n", cmd.c_str());
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return false;
    char buf[4096];
    size_t n;
    while (out->size() < 65536 && (n = fread(buf, 1, sizeof buf, fp)) > 0)
      out->append(buf, n);
    int rc = pclose(fp);
    printf("[EV] exec rc=%d, %zu bytes\n", rc, out->size());
    return true;
  }

  static int Query(const std::string& plate, std::string* detail) {
    const char* kUrl = cfg::kEvLookupUrl;   // config.h 단일 출처 (URL 하드코딩 금지)
    std::string body = "selectCarNum=1&searchWord=" + PctEncode(plate);

    // 카메라 내장 HTTP 클라이언트 후보를 순서대로 시도. 둘 다 TLS 는 자식
    // 프로세스 몫 — 앱 프로세스는 파이프로 결과만 받는다.
    std::string cmds[2];
    cmds[0] = std::string("curl -sk --max-time 8 -A Mozilla/5.0 --data '") +
              body + "' " + kUrl + " 2>&1";
    cmds[1] = std::string("wget -q -O - --no-check-certificate -T 8 --post-data '") +
              body + "' " + kUrl + " 2>&1";

    std::string resp, first_err;
    for (const auto& cmd : cmds) {
      if (!Exec(cmd, &resp)) {
        if (first_err.empty()) first_err = "popen failed (no fork?)";
        continue;
      }
      if (resp.find("\"data\"") != std::string::npos) {  // JSON 응답 확보
        if (resp.find("\"data\":null") != std::string::npos) {
          *detail = "not in low-emission list (ICE)";
          return 0;
        }
        std::string type = JsonStr(resp, "CAR_TYPE_NM");
        if (type.empty()) { *detail = "response parse failed"; return -1; }
        *detail = JsonStr(resp, "MAKR_NM") + " " + JsonStr(resp, "CAR_NM") + ", " +
                  JsonStr(resp, "USEFUELNM") + ", " + type;
        return type == "1종" ? 1 : 0;  // 1종=전기·수소, 2종(하이브리드)·3종은 아님
      }
      // JSON 이 안 왔다 — 바이너리 부재("not found")든 통신 실패든 다음 후보로
      if (first_err.empty())
        first_err = resp.empty() ? "no output" : resp.substr(0, 120);
    }
    *detail = "curl/wget lookup failed: " + first_err;
    return -1;
  }

  Callback cb_;
  std::thread th_;
  std::mutex mtx_;
  std::condition_variable cv_;
  std::deque<Job> q_;
  bool run_ = false;
};
