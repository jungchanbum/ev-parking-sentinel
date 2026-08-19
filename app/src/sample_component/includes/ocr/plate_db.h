#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

// ============================================================================
// PlateDb — 등록차량 목록 최근접 매칭 (편집거리 ≤1).
//   실전의 "아파트/주차장 등록차량 목록"에 해당하는 정당한 기능. 목록은 외부 파일
//   (한 줄에 번호판 하나, UTF-8)에서 로드 — 코드는 정답을 모른다. 파일이 없으면
//   레이어가 자동으로 꺼져 순수 인식 모드가 된다.
//   용도: ① HOLD 로 끝날 판을 등록 번호와 1글자 차이면 회수(db-rescue)
//        ② FINAL 텍스트가 미등록인데 유일 1글자 매칭이 있으면 교정(db-fix)
//   tinyLPR 오독의 대부분이 1글자 치환/삽입이라(실측: 42주8120, 115소5746, 09거2645)
//   편집거리 1(치환·삽입·삭제)이 정확히 그 영역을 덮는다. 복수 매칭은 애매 → 불개입.
// ============================================================================
class PlateDb {
 public:
  // 한 줄 형식: "번호[,플래그...]" — 예: "15소5746" / "12가3456,ev" (전기차).
  //   플래그 없는 기존 형식과 하위호환. 플래그는 차량365 백엔드가 등록 시점에
  //   외부 조회(등록원부 fuelType 등)로 확정해 써 넣는 값이다.
  bool Load(const std::string& path) {
    plates_.clear();
    raw_.clear();
    ev_.clear();
    std::ifstream ifs(path);
    if (!ifs) return false;
    std::string line;
    while (std::getline(ifs, line)) {
      while (!line.empty() &&
             (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
        line.pop_back();
      if (line.empty() || line[0] == '#') continue;   // 주석/빈 줄 허용
      std::string plate = line, flags;
      size_t comma = line.find(',');
      if (comma != std::string::npos) {
        plate = line.substr(0, comma);
        flags = line.substr(comma + 1);
      }
      if (plate.empty()) continue;
      plates_.push_back(Decode(plate));
      raw_.push_back(plate);
      ev_.push_back(flags.find("ev") != std::string::npos);
    }
    return !plates_.empty();
  }

  // 등록 번호(raw 텍스트 정확 일치)의 전기차 플래그. 미등록이면 false.
  bool IsEv(const std::string& plate) const {
    for (size_t i = 0; i < raw_.size(); i++)
      if (raw_[i] == plate) return ev_[i];
    return false;
  }

  int size() const { return (int)raw_.size(); }

  // 반환: 0=매칭 없음, 1=유일 매칭(*out 에 등록 번호), 2=복수 매칭(애매 → 불개입).
  //   정확 일치는 즉시 유일 매칭으로 확정.
  //   원문이 안 맞고 선두가 지역명 꼴(한글2+숫자)이면 지역명을 벗겨서 한 번 더 대조 —
  //   tinyLPR 이 판 가장자리를 보고 "경기36라7833"처럼 지역명을 환각하는 케이스 회수.
  //   (진짜 지역판 등록차는 DB에 지역명 포함으로 넣으면 원문 대조에서 먼저 걸린다)
  int Match(const std::string& text, std::string* out) const {
    std::vector<uint32_t> t = Decode(text);
    int r = MatchCp(t, out);
    if (r == 0 && t.size() >= 3 && IsHangul(t[0]) && IsHangul(t[1]) && IsDigit(t[2]))
      r = MatchCp(std::vector<uint32_t>(t.begin() + 2, t.end()), out);
    return r;
  }

  // [지역 복원 08-14] 꼬리(숫자시작 일반형) 판독 → 명부의 지역판 완성번호로 승격.
  //   1단(세로 지역명) 택시판은 모델이 지역 레이아웃을 학습 못 해 꼬리만 읽힌다
  //   (실측: 이미지 재조립·NCC 분류 전부 불안정). 지역의 진실은 명부가 안다 —
  //   "지역명은 고정 DB" 설계. 명부에 [지역2글자]+꼬리 꼴 등록번호가 정확히 하나면
  //   그 번호로 복원. 복수면 애매 → 불개입. 반환: 복원 성공 여부.
  // 텍스트 선두가 지역명 꼴(한글2+숫자)인가 — 일반판은 숫자 시작이라 오탐 불가.
  static bool HasRegionPrefix(const std::string& text) {
    std::vector<uint32_t> t = Decode(text);
    return t.size() >= 9 && IsHangul(t[0]) && IsHangul(t[1]) && IsDigit(t[2]);
  }

  bool RecoverRegion(const std::string& text, std::string* out, int* diff_out = nullptr) const {
    std::vector<uint32_t> t = Decode(text);
    // 선두가 지역명 꼴(한글2+숫자)이면 벗긴다 — 모델이 세로 지역기둥을 엉뚱한
    //   지역토큰("경기"/"대구" 등)으로 읽는 실측 케이스. 지역의 진실은 명부가 정한다.
    if (t.size() >= 9 && IsHangul(t[0]) && IsHangul(t[1]) && IsDigit(t[2]))
      t.erase(t.begin(), t.begin() + 2);
    if (t.size() < 7 || !IsDigit(t[0])) return false;   // 꼬리(일반형)만 대상
    int found = 0, best_diff = 3;
    for (size_t i = 0; i < plates_.size(); i++) {
      const auto& p = plates_[i];
      if (p.size() != t.size() + 2) continue;
      if (!IsHangul(p[0]) || !IsHangul(p[1])) continue;  // 선두 지역명 꼴만
      int diff = 0;                     // 꼬리 치환 ≤2 (지역기둥이 폭을 뺏어 2글자 오독 실측:
      for (size_t k = 0; k < t.size() && diff <= 2; k++)  //  3372→3337, 아→어)
        if (p[k + 2] != t[k]) ++diff;
      if (diff <= 2) {
        if (found == 0) { *out = raw_[i]; best_diff = diff; }
        found++;
      }
    }
    if (found == 1 && diff_out) *diff_out = best_diff;
    return found == 1;
  }

  // [tier-2: 2글자 회수] 같은 길이에서 치환 ≤2 인 등록 번호가 정확히 하나면 그 번호.
  //   저해상 리그의 표준 오독이 혼동쌍(0↔9, 5↔6, 1↔7, 허↔머) 2글자 패턴 (08-06 실측:
  //   90머5755→96머5715, 40허5578→49허5678 — 전부 ed2 라 tier-1 그물 밖이었음).
  //   안전장치: 후보가 2개 이상이면 애매 → 불개입 (반환 2). 삽입/삭제는 안 봄 (길이 동일만).
  int MatchLoose(const std::string& text, std::string* out) const {
    std::vector<uint32_t> t = Decode(text);
    int r = MatchLooseCp(t, out);
    if (r == 0 && t.size() >= 3 && IsHangul(t[0]) && IsHangul(t[1]) && IsDigit(t[2]))
      r = MatchLooseCp(std::vector<uint32_t>(t.begin() + 2, t.end()), out);
    return r;
  }

 private:
  static bool IsHangul(uint32_t cp) { return cp >= 0xAC00 && cp <= 0xD7A3; }
  static bool IsDigit(uint32_t cp) { return cp >= '0' && cp <= '9'; }

  int MatchLooseCp(const std::vector<uint32_t>& t, std::string* out) const {
    int found = 0;
    for (size_t i = 0; i < plates_.size(); i++) {
      const auto& p = plates_[i];
      if (p.size() != t.size()) continue;    // 길이 다르면 제외 (치환만 인정)
      int diff = 0;
      for (size_t k = 0; k < t.size() && diff <= 2; k++)
        if (t[k] != p[k]) ++diff;
      if (diff <= 2) {
        if (found == 0) *out = raw_[i];
        found++;
      }
    }
    return found == 0 ? 0 : (found == 1 ? 1 : 2);
  }

  int MatchCp(const std::vector<uint32_t>& t, std::string* out) const {
    int found = 0;
    for (size_t i = 0; i < plates_.size(); i++) {
      int d = Dist01(t, plates_[i]);
      if (d == 0) { *out = raw_[i]; return 1; }
      if (d == 1) {
        if (found == 0) *out = raw_[i];
        found++;
      }
    }
    return found == 0 ? 0 : (found == 1 ? 1 : 2);
  }

  // UTF-8 → 코드포인트 배열 (한글 1글자 = 1요소가 되도록)
  static std::vector<uint32_t> Decode(const std::string& s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
      unsigned char c = s[i];
      uint32_t cp = c;
      int n = 1;
      if (c >= 0xF0) { cp = c & 0x07; n = 4; }
      else if (c >= 0xE0) { cp = c & 0x0F; n = 3; }
      else if (c >= 0xC0) { cp = c & 0x1F; n = 2; }
      for (int k = 1; k < n && i + k < s.size(); k++)
        cp = (cp << 6) | (s[i + k] & 0x3F);
      out.push_back(cp);
      i += n;
    }
    return out;
  }

  // 편집거리 0/1 판정: 0=일치, 1=치환·삽입·삭제 1회, 2=그 이상
  static int Dist01(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    size_t la = a.size(), lb = b.size();
    if (la == lb) {                       // 치환 검사
      int diff = 0;
      for (size_t i = 0; i < la; i++)
        if (a[i] != b[i] && ++diff > 1) return 2;
      return diff;
    }
    const std::vector<uint32_t>& s = la < lb ? a : b;   // 짧은 쪽
    const std::vector<uint32_t>& l = la < lb ? b : a;   // 긴 쪽
    if (l.size() - s.size() != 1) return 2;             // 길이차 2+ = 탈락
    size_t i = 0, j = 0; bool skipped = false;          // 삽입/삭제 1회 허용 스캔
    while (i < s.size() && j < l.size()) {
      if (s[i] == l[j]) { i++; j++; continue; }
      if (skipped) return 2;
      skipped = true; j++;                              // 긴 쪽 한 글자 건너뜀
    }
    return 1;
  }

  std::vector<std::vector<uint32_t>> plates_;
  std::vector<std::string> raw_;
  std::vector<bool> ev_;   // raw_ 와 같은 인덱스 — 전기차 플래그 (파일의 ",ev")
};
