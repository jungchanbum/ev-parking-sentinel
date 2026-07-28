#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

#include <opencv2/imgproc.hpp>
#include <opencv2/photo.hpp>   // fastNlMeansDenoising — 2단계 디노이즈 재판독

#include "config.h"
#include "ocr/plate_decode.h"
#include "ocr/tflite_model.h"

// ============================================================================
// PlateOcr — 멀티후보 트리밍 + tinyLPR 단독 (PC ocr_lab 검증 로직 그대로).
//   WiseAI 크롭은 여백이 많아(헐렁) 모델이 헛읽음(17%) → 후보 크롭 6~8개를 만들어
//   전부 tinyLPR 로 읽고 '유효포맷 > 신뢰도'로 선택하면 86% (candidates_test.py 검증).
//   tinyLPR 가 ~4ms 라 후보 전수 OCR 이 실전에서도 감당됨(~30ms).
//   Multi-line 병합은 실측 기여 0(어블레이션 + 실전 로그)으로 제거 — 경량화.
// ============================================================================
struct PlateOcrResult {
  std::string text;              // 최종 결과(무효면 "")
  double confidence = 0.0;        // 신뢰도 (비블랭크 argmax 확률의 기하평균)
  std::string source;             // 디버그: "tiny(valid)" | "jitter" | "denoise" 등
  std::string color = "?";        // 번호판 배경색 분류: wh/ye/gr/bl/? (색-한글 검증용)
  // ---- 진단(디버그 뷰어용): 이긴 후보 기준 결과 + 지연시간(병목 확인) ----
  std::string tiny_text;   double tiny_conf = 0.0;   double tiny_ms = 0.0;  // ms는 후보 전체 합
  double denoise_ms = 0.0;  // 2단계 디노이즈+재판독 소요 (병목 감시 — NlMeans 는 무거운 편)
  double crop_lum = 0.0;    // 이긴 크롭의 평균 밝기 (조도 진단 — 쌍라이트/역광 확인용)
  double crop_lum_fixed = 0.0;  // 조도 정규화 후 평균 밝기 (0 = 보정 미발동)
  int cand_total = 0;       // 시도한 후보 크롭 수
  double total_ms = 0.0;    // Recognize() 전체 (후보생성+전수 OCR)
};

class PlateOcr {
 public:
  bool Load(const std::string& kr_model, const std::string& kr_labels) {
    // 스레드 수는 config 참고 — 2 는 스레드풀 스핀이 2코어를 독점해 앱 동결(실측).
    if (!kr_.Load(kr_model, cfg::kTfliteThreads)) return false;
    kr_chars_ = plate_decode::LoadChars(kr_labels);
    return !kr_chars_.empty();
  }

  // light=true: 버스트용 경량 파이프라인 — 후보 3개(원본+밝은박스1+중앙1), 지터·조도·
  //   구조대 전부 생략. 버스트는 "참고 표"라 풀코스가 과했음(부하 다이어트 2026-07-28).
  //   good-shot(본선수)은 항상 풀코스(light=false).
  PlateOcrResult Recognize(const cv::Mat& crop_bgr_or_gray, bool light = false) {
    using clk = std::chrono::steady_clock;
    auto ms_since = [](clk::time_point t0) {
      return std::chrono::duration<double, std::milli>(clk::now() - t0).count();
    };
    auto t_all = clk::now();

    cv::Mat gray;
    if (crop_bgr_or_gray.channels() == 3)
      cv::cvtColor(crop_bgr_or_gray, gray, cv::COLOR_BGR2GRAY);
    else
      gray = crop_bgr_or_gray;

    PlateOcrResult r;

    // ---- 번호판 배경색 분류 (밝은 픽셀 = 판 배경의 평균색) → 색-한글 검증에 사용 ----
    r.color = ClassifyPlateColor(crop_bgr_or_gray, gray);
    // "유효" = 포맷 OK + 색-한글 모순 없음 (노랑판만 아바사자배, 그 외 판에선 금지)
    auto validc = [&](const std::string& t) {
      return plate_decode::ValidPlateFormat(t) &&
             (!cfg::kColorRule || ColorConsistent(t, r.color));
    };

    // ---- 후보 크롭 전수 OCR(tinyLPR) → 유효(포맷+색) > 신뢰도 로 최고 후보 선택 ----
    std::vector<cv::Mat> cands = MakeCandidates(gray, light);
    r.cand_total = (int)cands.size();
    cv::Mat best_crop = gray;
    std::string best_txt; double best_conf = -1.0; bool best_valid = false;
    auto t0 = clk::now();
    for (const cv::Mat& c : cands) {
      std::string txt; double conf;
      RunKr(c, &txt, &conf);
      bool v = validc(txt);
      if ((v && !best_valid) || (v == best_valid && conf > best_conf)) {
        best_valid = v; best_conf = conf; best_txt = txt; best_crop = c;
      }
    }
    r.tiny_ms = ms_since(t0);
    r.tiny_text = best_txt; r.tiny_conf = best_conf < 0 ? 0.0 : best_conf;

    r.text = r.tiny_text;
    r.confidence = r.tiny_conf;
    r.source = best_valid ? "tiny(valid)" : "tiny";

    // ---- 미세 지터 앙상블: 이긴 크롭을 ±수 px 재프레이밍해 재판독 (conf<0.98 만) ----
    //   레터박스 정렬이 2px 달라지면 경계 걸친 글자(7↔8, 라↔파)가 갈리는 것을 이용.
    //   재프레이밍은 화질 조작이 아니므로 캡 없음 (공간 후보와 동급 지위).
    // 봉합(2026-07-28): 원판독이 게이트를 넘는(=FINAL 자격) 경우엔 절대 개입 금지.
    //   지터가 정답 0.96 을 오답 0.94(캡)로 바꿔치기해 FINAL→오답 HOLD 로 만든 사고 실측
    //   (15노1199→15누1199, 25노5701→25누6701). 가공은 어차피 HOLD 인 판만 만진다.
    if (!light && cfg::kJitterEnsemble && r.confidence < cfg::kFinalConfFloor && best_crop.cols > 32) {
      static const double J[5][2] = {{-3, 0}, {3, 0}, {0, -2}, {0, 2}, {0, 0}};  // 마지막=스케일
      for (int j = 0; j < 5; j++) {
        cv::Mat moved;
        if (j < 4) {
          cv::Mat M = (cv::Mat_<double>(2, 3) << 1, 0, J[j][0], 0, 1, J[j][1]);
          cv::warpAffine(best_crop, moved, M, best_crop.size(),
                         cv::INTER_LINEAR, cv::BORDER_REPLICATE);
        } else {                                     // 6% 확대 후 중앙 원크기 크롭
          cv::Mat big; cv::resize(best_crop, big, cv::Size(0, 0), 1.06, 1.06, cv::INTER_LINEAR);
          int ox = (big.cols - best_crop.cols) / 2, oy = (big.rows - best_crop.rows) / 2;
          moved = big(cv::Rect(ox, oy, best_crop.cols, best_crop.rows)).clone();
        }
        std::string tj; double cj;
        RunKr(moved, &tj, &cj);
        bool vj = validc(tj);
        bool bv = validc(r.text);
        if ((vj && !bv) || (vj == bv && cj > r.confidence)) {
          // 지터도 conf 0.95 캡 — "재프레이밍이라 무해" 가설은 실측 반증됨
          // (오답 0.96 을 0.98 로 부스트해 게이트 통과, 26누5701 사건).
          // 선택 이후 conf 를 올리는 모든 장치는 캡 — 오늘 4번째 반복된 최종 교훈.
          r.text = tj; r.confidence = std::min(cj, cfg::kRescueConfCap);
          r.source = vj ? "jitter(valid)" : "jitter";
        }
      }
    }

    // ---- 2단계a: 조도 정규화 — 평균 밝기가 비정상이면 감마로 128 에 맞춰 재판독 ----
    //   쌍라이트에 속은 AE(어두운 번호판)·반사 과노출 대응. LUT 라 ~1ms.
    r.crop_lum = cv::mean(best_crop)[0];
    if (!light && cfg::kExposureNorm && r.confidence < cfg::kFinalConfFloor &&
        (r.crop_lum < cfg::kExposureLow || r.crop_lum > cfg::kExposureHigh)) {
      double m = std::min(std::max(r.crop_lum / 255.0, 0.02), 0.98);
      double g = std::log(128.0 / 255.0) / std::log(m);   // 평균이 128 로 가는 감마
      cv::Mat lut(1, 256, CV_8U);
      for (int i = 0; i < 256; i++)
        lut.at<uchar>(i) = cv::saturate_cast<uchar>(std::pow(i / 255.0, g) * 255.0);
      cv::Mat fixed;
      cv::LUT(best_crop, lut, fixed);
      r.crop_lum_fixed = cv::mean(fixed)[0];
      std::string t2; double c2;
      RunKr(fixed, &t2, &c2);
      bool v2 = validc(t2);
      bool bv = validc(r.text);
      if ((v2 && !bv) || (v2 == bv && c2 > r.confidence)) {
        // 보정 결과는 conf 0.95 캡 — 보정 부스트+primary 가중이 겹쳐 정답 버스트(1.00)를
        // 오답이 역전한 사고 실측(42주8120 사건). 디노이즈와 동일한 "구조 전용" 지위.
        r.text = t2; r.confidence = std::min(c2, cfg::kRescueConfCap);
        r.source = v2 ? "exposure(valid)" : "exposure";
        best_crop = fixed;   // 디노이즈 구조 단계도 보정본 기준으로
      }
    }

    // ---- 2단계b: 구조 체인 (conf<0.90 일 때만, 전부 conf 0.95 캡) ----
    //   보정 결과는 원본보다 신뢰할 수 없음 → 챔피언십엔 참가하되 단독으로는
    //   FINAL 게이트(0.96)를 못 넘게 한다 (오답 승격 실측: 09저2643·42주8120 사건).
    if (!light && r.confidence < 0.90) {
      t0 = clk::now();
      auto try_rescue = [&](const cv::Mat& img, const char* tag) {
        std::string t2; double c2;
        RunKr(img, &t2, &c2);
        bool v2 = validc(t2);
        bool bv = validc(r.text);
        if ((v2 && !bv) || (v2 == bv && c2 > r.confidence)) {
          r.text = t2; r.confidence = std::min(c2, cfg::kRescueConfCap);
          r.source = std::string(tag) + (v2 ? "(valid)" : "");
        }
      };
      // 약한 언샤프 — 노이즈 국면에선 독이었지만 BLC 이후 깨끗한 저대비 국면용 재실험
      cv::Mat blur_, sharp_;
      cv::GaussianBlur(best_crop, blur_, cv::Size(0, 0), 1.0);
      cv::addWeighted(best_crop, 1.5, blur_, -0.5, 0, sharp_);
      try_rescue(sharp_, "sharpen");
      // 디노이즈 (NlMeans, 무거움)
      if (cfg::kDenoise2ndPass) {
        cv::Mat dn;
        cv::fastNlMeansDenoising(best_crop, dn, 5.0f, 7, 15);
        try_rescue(dn, "denoise");
      }
      // 채널 라우팅 — 유색판(초록/파랑/노랑)일 때 배경이 가장 밝은 채널로 재판독
      if (crop_bgr_or_gray.channels() == 3 && r.color != "wh" && r.color != "?") {
        int ch = r.color == "bl" ? 0 : (r.color == "gr" ? 1 : 2);  // ye→R
        cv::Mat chan;
        cv::extractChannel(crop_bgr_or_gray, chan, ch);
        try_rescue(chan, "channel");
      }
      r.denoise_ms = ms_since(t0);   // 구조 체인 전체 소요
    }

    r.total_ms = ms_since(t_all);
    return r;
  }

 private:
  // ---- 번호판 배경색 분류: 밝은 픽셀(=판 배경)의 평균색을 HSV 로 판정 ----
  //   반환: "wh"(흰/저채도) "ye"(노랑) "gr"(초록) "bl"(파랑) "?"(판정불가)
  //   채도가 낮으면 무조건 wh — AWB 색쏠림으로 흰판을 유색으로 오판하는 것 방지.
  static std::string ClassifyPlateColor(const cv::Mat& bgr, const cv::Mat& gray) {
    if (bgr.channels() != 3) return "?";
    double mn, mx;
    cv::minMaxLoc(gray, &mn, &mx);
    double thr = (cv::mean(gray)[0] + mx) * 0.5;      // 상위 밝기 영역 = 판 배경
    cv::Mat mask = gray >= thr;
    if (cv::countNonZero(mask) < 30) return "?";
    cv::Scalar m = cv::mean(bgr, mask);
    cv::Mat px(1, 1, CV_8UC3, cv::Vec3b((uchar)m[0], (uchar)m[1], (uchar)m[2]));
    cv::Mat hsv_m; cv::cvtColor(px, hsv_m, cv::COLOR_BGR2HSV);
    cv::Vec3b hsv = hsv_m.at<cv::Vec3b>(0, 0);
    int H = hsv[0], S = hsv[1];
    if (S < 55) return "wh";                          // 저채도 = 흰판 (확실할 때만 유색 판정)
    if (H >= 18 && H < 42) return "ye";
    if (H >= 42 && H < 90) return "gr";
    if (H >= 90 && H < 135) return "bl";
    return "?";
  }

  // ---- 색-한글 일관성: 영업용 글자(아바사자배)는 노랑판 전용 — 법정 규격 ----
  //   흰판에서 "27아8257" 같은 환각을 즉시 기각. 색이 "?" 면 제약 없음.
  static bool ColorConsistent(const std::string& text, const std::string& color) {
    if (color == "?") return true;
    static const char* kBiz[] = {"아", "바", "사", "자", "배"};
    bool has_biz = false;
    for (size_t i = 0; i + 2 < text.size(); i++) {
      unsigned char b = text[i];
      if (b >= 0xEA && b <= 0xED) {                   // UTF-8 한글 3바이트 시작
        std::string hangul = text.substr(i, 3);
        for (const char* bz : kBiz)
          if (hangul == bz) { has_biz = true; break; }
        break;                                        // 첫 한글만 검사 (번호판엔 1글자)
      }
    }
    return color == "ye" ? has_biz : !has_biz;
  }

  // ---- 후보 크롭 생성 (candidates_test.py 와 동일 파라미터) ----
  //   ①원본  ②밝은영역 박스 top2 × 마진 2종  ③중앙크롭 3종  = 최대 8개
  // light: 후보를 3개로 압축 (원본 + 밝은박스 top1×타이트마진 + 중앙 1종) — 버스트용
  static std::vector<cv::Mat> MakeCandidates(const cv::Mat& gray, bool light = false) {
    std::vector<cv::Mat> out;
    out.push_back(gray);

    // 밝은영역(흰 번호판) 후보: Otsu 이진화 → 가로로 닫기 → 번호판 비율 사각형
    cv::Mat g, th;
    cv::GaussianBlur(gray, g, cv::Size(5, 5), 0);
    cv::threshold(g, th, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
    cv::morphologyEx(th, th, cv::MORPH_CLOSE,
                     cv::getStructuringElement(cv::MORPH_RECT, cv::Size(17, 5)));
    std::vector<std::vector<cv::Point>> cnts;
    cv::findContours(th, cnts, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    const int W = gray.cols, H = gray.rows;
    std::vector<std::pair<int, cv::Rect>> boxes;  // (area, rect)
    for (const auto& c : cnts) {
      cv::Rect b = cv::boundingRect(c);
      if (b.height == 0) continue;
      double ar = (double)b.width / b.height;
      int area = b.width * b.height;
      if (ar >= 1.5 && ar <= 8.0 && area > 0.04 * W * H) boxes.push_back({area, b});
    }
    std::sort(boxes.begin(), boxes.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    const double margins[2][2] = {{0.06, 0.15}, {0.14, 0.35}};  // (가로, 세로) 마진 비율
    const size_t max_boxes = light ? 1 : 2;
    const size_t max_margins = light ? 1 : 2;
    for (size_t i = 0; i < boxes.size() && i < max_boxes; i++) {
      const cv::Rect& b = boxes[i].second;
      for (size_t mi = 0; mi < max_margins; mi++) {
        const double* m = margins[mi];
        int mx = (int)(b.width * m[0]), my = (int)(b.height * m[1]);
        cv::Rect rc(std::max(0, b.x - mx), std::max(0, b.y - my), 0, 0);
        rc.width = std::min(W, b.x + b.width + mx) - rc.x;
        rc.height = std::min(H, b.y + b.height + my) - rc.y;
        if (rc.width >= 16 && rc.height >= 8) out.push_back(gray(rc));
      }
    }

    // 중앙크롭 (번호판이 크롭 중앙에 있는 경향) — light 는 1종만
    const double centers[3][2] = {{0.7, 0.45}, {0.55, 0.35}, {0.85, 0.6}};
    const size_t max_centers = light ? 1 : 3;
    for (size_t ci = 0; ci < max_centers; ci++) {
      int cw = (int)(W * centers[ci][0]), ch = (int)(H * centers[ci][1]);
      if (cw >= 16 && ch >= 8)
        out.push_back(gray(cv::Rect((W - cw) / 2, (H - ch) / 2, cw, ch)));
    }
    return out;
  }

  // KR_LPR(tinyLPR): letterbox(top-left, 종횡비 유지) + CTC.
  void RunKr(const cv::Mat& gray, std::string* text, double* conf) {
    int H, W; kr_.InputHW(&H, &W);
    double r = std::min((double)W / gray.cols, (double)H / gray.rows);
    cv::Mat resized; cv::resize(gray, resized, cv::Size(0, 0), r, r, cv::INTER_AREA);
    cv::Mat canvas = cv::Mat::zeros(H, W, CV_8UC1);
    resized.copyTo(canvas(cv::Rect(0, 0, std::min(resized.cols, W), std::min(resized.rows, H))));
    cv::Mat f; canvas.convertTo(f, CV_32F, 1.0 / 255.0);
    f = f.isContinuous() ? f : f.clone();

    std::vector<float> out; int T, C;
    if (!kr_.Run(f.ptr<float>(), (size_t)H * W, &out, &T, &C)) { *text = ""; *conf = 0; return; }
    std::vector<int> idx; std::vector<float> prob;
    plate_decode::ArgmaxPerStep(out, T, C, &idx, &prob);
    *text = plate_decode::CtcCollapse(idx, kr_chars_);
    *conf = plate_decode::Confidence(idx, prob, (int)kr_chars_.size());
  }

  TfliteModel kr_;
  std::vector<std::string> kr_chars_;
};
