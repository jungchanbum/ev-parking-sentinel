#pragma once

#include <cmath>
#include <fstream>
#include <string>
#include <vector>

// ============================================================================
// PlateDecode — CTC/직접분류 디코딩 + 신뢰도 계산 + 번호판 포맷 검증.
//   ocr_lab(PC, Python)에서 검증한 tinylpr_test.py/confidence_merge.py 로직을
//   그대로 C++로 옮김(groupby collapse, 기하평균 신뢰도, 정규식 없는 포맷 체크).
// ============================================================================
namespace plate_decode {

// label.names(줄마다 문자 하나) 로드. index i → chars[i]. CTC blank = chars.size().
inline std::vector<std::string> LoadChars(const std::string& path) {
  std::vector<std::string> chars;
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    chars.push_back(line);
  }
  return chars;
}

// out=[T,C] 에서 timestep별 argmax 인덱스/확률. C=blank 포함 클래스 수.
inline void ArgmaxPerStep(const std::vector<float>& out, int T, int C,
                          std::vector<int>* idx, std::vector<float>* prob) {
  idx->resize(T); prob->resize(T);
  // 값 범위로 logits/확률 판별 → logits면 그 스텝만 softmax
  bool looks_prob = true;
  for (float v : out) if (v < -1e-3f || v > 1.001f) { looks_prob = false; break; }
  for (int t = 0; t < T; t++) {
    const float* row = &out[t * C];
    int best = 0; float bestv = row[0];
    for (int c = 1; c < C; c++) if (row[c] > bestv) { bestv = row[c]; best = c; }
    if (!looks_prob) {  // softmax(row)[best] 만 필요 → 안정적 계산
      float mx = row[0]; for (int c = 1; c < C; c++) mx = std::max(mx, row[c]);
      double sum = 0; for (int c = 0; c < C; c++) sum += std::exp((double)row[c] - mx);
      bestv = (float)(std::exp((double)bestv - mx) / sum);
    }
    (*idx)[t] = best; (*prob)[t] = bestv;
  }
}

// CTC 디코딩: 연속 반복 제거 + blank(index==chars.size()) 제거 → 문자열.
inline std::string CtcCollapse(const std::vector<int>& idx, const std::vector<std::string>& chars) {
  std::string out; int prev = -1;
  for (int k : idx) {
    if (k != prev) { if (k != (int)chars.size() && k >= 0 && k < (int)chars.size()) out += chars[k]; }
    prev = k;
  }
  return out;
}

// 직접분류(비-CTC): collapse 없이 매 스텝 그대로 이어붙임(Multi-line 폴백용).
inline std::string DirectJoin(const std::vector<int>& idx, const std::vector<std::string>& chars) {
  std::string out;
  for (int k : idx) if (k >= 0 && k < (int)chars.size()) out += chars[k];
  return out;
}

// 신뢰도 = collapse 후 남은(=blank 아닌) 스텝들 확률의 기하평균.
inline double Confidence(const std::vector<int>& idx, const std::vector<float>& prob, int blank) {
  double sum_log = 0; int n = 0;
  for (size_t t = 0; t < idx.size(); t++) {
    if (idx[t] == blank) continue;
    sum_log += std::log(std::max(1e-6, (double)prob[t])); n++;
  }
  if (n == 0) return 0.0;
  return std::exp(sum_log / n);
}

// 번호판 포맷 검증: 숫자(2~3) + 한글(1, UTF-8 3바이트) + 숫자(4). 정규식 없이 바이트 파싱.
//   (LoadChars 의 문자셋이 숫자/한글로만 curated 돼 있다는 전제 — 그 외 바이트는 무효 처리)
inline bool ValidPlateFormat(const std::string& s) {
  std::vector<int> kinds;  // 0=숫자(1B), 1=한글(3B)
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) {
      if (c < '0' || c > '9') return false;
      kinds.push_back(0); i += 1;
    } else if ((c & 0xF0) == 0xE0) {
      if (i + 2 >= s.size()) return false;
      kinds.push_back(1); i += 3;
    } else {
      return false;
    }
  }
  int n = (int)kinds.size();
  std::vector<int> hp;                       // 한글 위치들
  for (int j = 0; j < n; j++)
    if (kinds[j] == 1) hp.push_back(j);
  // 일반판: 숫자(2~3) + 한글 + 숫자(4)  — 12가3456, 123가4568
  if (hp.size() == 1) {
    int before = hp[0], after = n - hp[0] - 1;
    return (before == 2 || before == 3) && after == 4;
  }
  // 지역판(한 줄): 한글2(지역명) + 숫자(1~3) + 한글 + 숫자(4) — 서울12가3456, 경기27바8257
  //   (두 줄 구형 지역판은 tinyLPR 이 한 줄 모델이라 별개 한계)
  if (hp.size() == 3 && hp[0] == 0 && hp[1] == 1) {
    int mid = hp[2] - 2, after = n - hp[2] - 1;
    return mid >= 1 && mid <= 3 && after == 4;
  }
  return false;
}

}  // namespace plate_decode
