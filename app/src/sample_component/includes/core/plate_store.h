#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "config.h"

// ============================================================================
// PlateStore — 카메라(WiseAI)가 준 번호판 크롭(ImageRef)을 읽어 순환버퍼에 저장.
//   WiseAI 가 이미 "최적 샷(best-shot)"을 1장 골라 주므로, 앱은 별도 선명도 비교
//   없이 오는 크롭을 그대로 저장한다(차 1대 = 1장, 중복만 제거).
//   늦게 써지는 파일은 pending 재시도. SDK/이벤트 무관: 알림은 문자열로 반환.
// ============================================================================
class PlateStore {
 public:
  // 새 크롭이면 저장. 저장 시 알림 문자열(없으면 ""). now_ms: pending 타임아웃용.
  std::string Save(int ch, const std::string& imgref, uint64_t now_ms) {
    if (ch < 0 || ch >= cfg::kChannels) return "";
    if (imgref.empty() || imgref == last_ref_[ch]) return "";

    long oid = ParseObjectId(imgref);
    bool same_car = (oid != 0 && oid == last_oid_[ch]);  // 같은 번호판 객체의 갱신
    int slot = same_car ? last_slot_[ch] : (total_ % cfg::kRingSize);

    std::string data;
    if (!ReadCompleteJpeg(imgref, data)) {  // 파일 아직 안 써짐 → 재시도 등록
      if (pending_ref_[ch] != imgref) {
        pending_ref_[ch] = imgref; pending_slot_[ch] = slot;
        pending_same_[ch] = same_car; pending_oid_[ch] = oid;
        pending_ms_[ch] = now_ms; read_fail_++;
      }
      return "";
    }
    if (pending_ref_[ch] == imgref) pending_ref_[ch].clear();
    return Commit(ch, imgref, slot, oid, same_car, data, /*retry=*/false);
  }

  // 늦게 써진 파일 재시도(최대 3초). 저장 시 알림 문자열(없으면 "").
  std::string RetryPending(int ch, uint64_t now_ms) {
    if (ch < 0 || ch >= cfg::kChannels || pending_ref_[ch].empty()) return "";
    std::string data;
    if (ReadCompleteJpeg(pending_ref_[ch], data)) {
      std::string ref = pending_ref_[ch];
      std::string ev = Commit(ch, ref, pending_slot_[ch], pending_oid_[ch],
                              pending_same_[ch], data, /*retry=*/true);
      pending_ref_[ch].clear();
      return ev;
    } else if (now_ms - pending_ms_[ch] > 3000) {
      pending_ref_[ch].clear();  // 타임아웃 포기
    }
    return "";
  }

  // [good-shot 재요청 효과] 중복제거 기억을 지운다. WiseAI 는 객체당 good-shot 을
  //   한 번만 "알려"주지만 ImageRef 경로는 메타데이터에 계속 실려온다 — 이 기억을
  //   지우면 같은 크롭을 새것처럼 다시 받아 판독을 처음부터 재시작한다 (재요청 API 없음).
  void ForgetRefs(int ch) {
    if (ch < 0 || ch >= cfg::kChannels) return;
    last_ref_[ch].clear();
    last_oid_[ch] = 0;
  }

  int total() const { return total_; }
  int read_fail() const { return read_fail_; }
  const int* saved_ch() const { return saved_ch_; }
  // 그 채널이 방금 쓴 슬롯 번호 — OCR 이 cap_<slot>.jpg 를 읽어들일 때 사용.
  int last_slot(int ch) const { return (ch >= 0 && ch < cfg::kChannels) ? last_slot_[ch] : -1; }

  // [갤러리] 임의 JPEG(버스트/베스트프레임 크롭 등)를 다음 링 슬롯에 저장 — /plate?n= 확인용.
  //   ImageRef 경로와 무관하게 "OCR 에 실제 들어간 크롭"을 눈으로 보게 하는 진단 저장.
  int SaveDebugImage(int ch, const std::string& jpg) {
    if (jpg.empty()) return -1;
    int slot = total_ % cfg::kRingSize;
    WritePair(jpg, slot);
    last_slot_[ch] = slot;
    total_++;
    if (ch >= 0 && ch < cfg::kChannels) saved_ch_[ch]++;
    return slot;
  }
  // 그 슬롯의 번호판 객체 id — good-shot OCR 결과를 시간투표에 합류시킬 때 사용.
  long last_oid(int ch) const { return (ch >= 0 && ch < cfg::kChannels) ? last_oid_[ch] : 0; }

  // [검증] 최근 저장 후보 기록 (원본 크롭 확인용). /candlist 가 JSON 으로 노출.
  struct CandRec { long seq; int file; int slot; int ch; long oid; double score; char decision; };
  const std::deque<CandRec>& cand_log() const { return cand_log_; }

 private:
  // 새 차 → 저장(카운트). 같은 차 재프레임 → 같은 슬롯 덮어쓰기만(카운트 X).
  //   선명도 측정/거부/비교 없음 — WiseAI 가 준 크롭을 그대로 저장.
  std::string Commit(int ch, const std::string& imgref, int slot, long oid,
                     bool same_car, const std::string& data, bool retry) {
    // [검증모드] 원본 크롭을 cand_N.jpg 로도 덤프 (OCR 원본 비교용)
    int file = -1;
    if (cfg::kDebugCandidates) {
      file = (int)(cand_seq_ % cfg::kCandRing);
      char cp[80];
      snprintf(cp, sizeof(cp), "%s/cand_%d.jpg", cfg::kStorageDir, file);
      std::ofstream ofs(cp, std::ios::binary);
      if (ofs) ofs.write(data.data(), data.size());
    }

    std::string tosave = MaybeEnhance(data);  // kEnhanceBlur=false 면 원본 그대로
    if (!WritePair(tosave, slot)) return "";
    last_ref_[ch] = imgref;

    if (cfg::kDebugCandidates) {
      cand_log_.push_back({cand_seq_, file, slot, ch, oid, 0.0, same_car ? 'U' : 'N'});
      while ((int)cand_log_.size() > cfg::kCandRing) cand_log_.pop_front();
      cand_seq_++;
    }

    if (same_car) return "";  // 같은 차 재프레임 → 슬롯 덮어쓰기만, 알림/카운트 없음

    last_oid_[ch] = oid; last_slot_[ch] = slot;
    total_++; saved_ch_[ch]++;
    char m[160];
    snprintf(m, sizeof(m), "plate SAVE  #%d (total %d%s) ch%d oid%ld",
             slot, total_, retry ? ", retry" : "", ch, oid);
    return m;
  }

  // 블러 완화(언샤프 마스크). kEnhanceBlur=false 거나 실패면 원본 그대로.
  static std::string MaybeEnhance(const std::string& jpeg) {
    if (!cfg::kEnhanceBlur) return jpeg;
    std::vector<uchar> buf(jpeg.begin(), jpeg.end());
    cv::Mat img = cv::imdecode(buf, cv::IMREAD_COLOR);
    if (img.empty()) return jpeg;
    cv::Mat blur, sharp;
    cv::GaussianBlur(img, blur, cv::Size(0, 0), cfg::kUnsharpSigma);
    cv::addWeighted(img, 1.0 + cfg::kUnsharpAmount, blur, -cfg::kUnsharpAmount, 0, sharp);
    std::vector<uchar> out;
    std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 92};
    if (!cv::imencode(".jpg", sharp, out, params)) return jpeg;
    return std::string(out.begin(), out.end());
  }

  // ImageRef 경로에서 번호판 객체 id 파싱: ".../objectid_<ID>_<타임스탬프>.jpg"
  static long ParseObjectId(const std::string& imgref) {
    size_t p = imgref.find("objectid_");
    if (p == std::string::npos) return 0;
    return atol(imgref.c_str() + p + 9);  // "objectid_" = 9글자
  }

  // /tmp+imgref 를 읽어 "완성된 JPEG(FFD8..FFD9)"이면 바이트를 out 에 채운다.
  //   아직 쓰는 중(부분파일)이면 false → 호출자가 pending 재시도.
  static bool ReadCompleteJpeg(const std::string& imgref, std::string& out) {
    std::string fp = std::string(cfg::kTmpPrefix) + imgref;
    std::ifstream ifs(fp.c_str(), std::ios::binary);
    if (!ifs) return false;
    std::ostringstream oss; oss << ifs.rdbuf();
    std::string d = oss.str();
    size_t n = d.size();
    if (!(n > 8 && (unsigned char)d[0] == 0xFF && (unsigned char)d[1] == 0xD8 &&
          (unsigned char)d[n - 2] == 0xFF && (unsigned char)d[n - 1] == 0xD9))
      return false;
    out.swap(d);
    return true;
  }

  // JPEG 바이트를 plate_last.jpg + cap_slot.jpg 로 저장.
  static bool WritePair(const std::string& d, int slot) {
    bool saved = false;
    auto save = [&](const char* path) {
      std::ofstream ofs(path, std::ios::binary);
      if (ofs) { ofs.write(d.data(), d.size()); saved = true; }
    };
    std::string lastp = std::string(cfg::kStorageDir) + "/plate_last.jpg";
    save(lastp.c_str());
    char cp[64]; snprintf(cp, sizeof(cp), "%s/cap_%d.jpg", cfg::kStorageDir, slot);
    save(cp);
    return saved;
  }

  std::string last_ref_[cfg::kChannels];             // 마지막 처리 ImageRef (정확 중복 방지)
  long last_oid_[cfg::kChannels] = {0, 0, 0, 0};     // 직전 저장 번호판 객체 id
  int  last_slot_[cfg::kChannels] = {0, 0, 0, 0};    // 그 객체가 쓴 슬롯
  long cand_seq_ = 0;                                // [검증] 후보 전역 카운터(cand 파일 idx)
  std::deque<CandRec> cand_log_;                     // [검증] 최근 저장 기록
  int  total_ = 0;                                   // 저장 총 개수(=다음 슬롯 계산)
  int  saved_ch_[cfg::kChannels] = {0, 0, 0, 0};     // 채널별 저장 수(진단)
  int  read_fail_ = 0;                               // 파일 못 읽어 pending 간 횟수(진단)
  // 늦게 써지는 파일 재시도
  std::string pending_ref_[cfg::kChannels];
  uint64_t pending_ms_[cfg::kChannels] = {0, 0, 0, 0};
  int  pending_slot_[cfg::kChannels] = {0, 0, 0, 0};
  bool pending_same_[cfg::kChannels] = {false, false, false, false};
  long pending_oid_[cfg::kChannels] = {0, 0, 0, 0};
};
