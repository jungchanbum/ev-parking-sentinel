#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "../config.h"

// ============================================================================
// ParkTune — 주차 판정 런타임 튜닝 (WiseAI commonanalyticssettings 의 우리 앱 버전).
//   목적: "숫자 하나 바꾸는데 빌드→영상정지→물리재부팅→설치 10분 + 오염 리스크"
//   제거 (08-06). GET/POST /parking_tune 으로 즉시 적용, storage 파일로 영속화.
//   초기값 = config.h 의 컴파일 타임 값 (파일 없으면 지금과 동일하게 동작).
// ============================================================================
struct ParkTune {
  double overlap = cfg::kParkOverlapFrac;      // 겹침 문턱 (양방향 max) — 진입/주차 후보
  uint64_t dwell_ms = cfg::kParkDwellMs;       // 정지+겹침 유지 → 주차 확정
  uint64_t grace_ms = cfg::kParkLeaveGraceMs;  // 부재 지속 → 부재 의심(육안검증 시작)
  double exit_cover = 0.10;                    // 점유차 겹침 이하 = 즉시 출차 (90% 이탈)
  int presence_miss = 3;      // 육안검증 연속 빈칸 이 횟수 → 출차 확정. 0 = 검증 끔
                              //   (부재 유예만으로 즉시 출차 — WiseAI 정지차 감지가
                              //    안정적일 때만. 최소 객체크기 12px 하향 후 가능해짐)
  uint64_t presence_period_ms = 1500;  // 육안검증 검사 간격
  double presence_conf = 0.60;         // 검증 "판 잔존" 인정 최소 conf (+ 번호 유사성)
  double burst_margin = 0.35;          // 버스트 사거리 — 칸 bbox 확장 비율
  int best_frame_mode = 1;             // 1=베스트 프레임 모드(08-11): 진입~주차 동안 버스트
                                       //   크롭 sharp×크기 점수만 기록, 주차완료 시 챔피언 1장만
                                       //   OCR. 0=끔(기존 매프레임 합의 판독).
  double best_min_sharp = 150;         // [쓰레기 필터 08-11] 이 선명도(라플라시안 분산) 미만
                                       //   크롭은 챔피언 후보에서 제외 + 갤러리 저장 안 함. 심한
                                       //   블러(멀리·초점나감)를 최종 OCR 에서 배제. 다 미달이면
                                       //   판독 보류(빈 챔피언). 0=끔.
                                       //   ★기본 600 은 너무 빡세 실크롭(실측 sharp≈488)까지 죽여
                                       //   전량 starving 실측(08-11) → 150 으로. 순수 쓰레기(≈300)
                                       //   와 실크롭 사이. 패널/‌API 로 현장 조정.
  double best_min_aspect = 2.0;        // [쓰레기 필터] 가로/세로 비율 하한 — 판이 아닌 오크롭
                                       //   (로고·배경, 정사각에 가까움) 제외. 실판 크롭은 2.5~3+.
  uint64_t goodshot_grace_ms = 0;      // 판독 시작 후 이만큼 버스트 보류(굿샷 우선). 기본 0=끔:
                                       //   진입부터 우리 스냅샷 캡처를 돌린다 (WiseAI 굿샷은
                                       //   크롭 모양이 깨져 오히려 해로움 — 08-11 실측). 굿샷을
                                       //   기다려보고 싶으면 이 값을 올려 실험 (패널에서 즉시).
  // ---- 직렬화 ----------------------------------------------------------
  std::string ToJson() const {
    char b[320];
    snprintf(b, sizeof(b),
             "{\"overlap\":%.2f,\"dwell_ms\":%llu,\"grace_ms\":%llu,"
             "\"exit_cover\":%.2f,\"presence_miss\":%d,\"presence_period_ms\":%llu,"
             "\"presence_conf\":%.2f,\"burst_margin\":%.2f,\"goodshot_grace_ms\":%llu,"
             "\"best_frame_mode\":%d,\"best_min_sharp\":%.0f,\"best_min_aspect\":%.2f}",
             overlap, (unsigned long long)dwell_ms, (unsigned long long)grace_ms,
             exit_cover, presence_miss, (unsigned long long)presence_period_ms,
             presence_conf, burst_margin, (unsigned long long)goodshot_grace_ms,
             best_frame_mode, best_min_sharp, best_min_aspect);
    return b;
  }

  // 부분 갱신: 본문에 있는 키만 반영. 범위 밖 값은 그 키만 무시.
  //   반환: 하나라도 반영됐으면 true.
  bool FromJson(const std::string& body) {
    bool any = false;
    any |= Num(body, "overlap", 0.30, 0.95, &overlap);
    any |= NumU(body, "dwell_ms", 500, 15000, &dwell_ms);
    any |= NumU(body, "grace_ms", 500, 30000, &grace_ms);
    any |= Num(body, "exit_cover", 0.02, 0.50, &exit_cover);
    any |= NumI(body, "presence_miss", 0, 10, &presence_miss);
    any |= NumU(body, "presence_period_ms", 500, 10000, &presence_period_ms);
    any |= Num(body, "presence_conf", 0.30, 0.95, &presence_conf);
    any |= Num(body, "burst_margin", 0.0, 1.0, &burst_margin);
    any |= NumU(body, "goodshot_grace_ms", 0, 10000, &goodshot_grace_ms);
    any |= NumI(body, "best_frame_mode", 0, 1, &best_frame_mode);
    any |= Num(body, "best_min_sharp", 0, 10000, &best_min_sharp);
    any |= Num(body, "best_min_aspect", 0, 10, &best_min_aspect);
    return any;
  }

  // ---- 영속화 (storage/parking_tune.txt — 한 줄 JSON) -------------------
  void Load(const std::string& path) {
    path_ = path;
    std::ifstream f(path.c_str());
    if (!f) return;                       // 파일 없음 = 컴파일 기본값
    std::string line;
    std::getline(f, line);
    FromJson(line);
  }
  void Save() const {
    if (path_.empty()) return;
    std::ofstream f(path_.c_str());
    if (f) f << ToJson() << "\n";
  }

 private:
  // "키":숫자 파싱 (따옴표 유무 관대). 범위 밖이면 무시.
  static bool Num(const std::string& b, const char* key, double lo, double hi, double* out) {
    size_t k = b.find(std::string("\"") + key + "\"");
    if (k == std::string::npos) k = b.find(key);
    if (k == std::string::npos) return false;
    size_t c = b.find(':', k);
    if (c == std::string::npos) return false;
    double v = atof(b.c_str() + c + 1);
    if (v < lo || v > hi) return false;
    *out = v;
    return true;
  }
  static bool NumU(const std::string& b, const char* key, uint64_t lo, uint64_t hi, uint64_t* out) {
    double v;
    if (!Num(b, key, (double)lo, (double)hi, &v)) return false;
    *out = (uint64_t)v;
    return true;
  }
  static bool NumI(const std::string& b, const char* key, int lo, int hi, int* out) {
    double v;
    if (!Num(b, key, (double)lo, (double)hi, &v)) return false;
    *out = (int)v;
    return true;
  }
  std::string path_;
};
