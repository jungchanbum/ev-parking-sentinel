#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ============================================================================
// MetadataParser — ONVIF 메타데이터(XML) → SDK 무관한 객체 구조로 변환하는 순수 파서.
//   문자열 스캔 파싱을 이 한 파일에 가둔다. 다른 카메라/포맷으로 옮길 땐 여기만 교체.
//   (도메인 로직 — 움직임 판정·저장·detect on/off — 은 여기 없다. 호출자 몫.)
// ============================================================================
namespace meta {

enum Class { kOther = 0, kHuman = 1, kVehicle = 2, kPlate = 3 };

// 파싱된 객체 하나 (좌표는 프레임 크기로 정규화된 0~1)
struct Object {
  long id = 0;
  int cls = kOther;
  bool has_center = false;           // 사람/차량만 true (유효 중심좌표 있음)
  float cx = 0, cy = 0;              // CenterOfGravity 원좌표 (움직임 추적용)
  float l = 0, t = 0, r = 0, b = 0;  // 바운딩박스 정규화 0~1
  long parent = 0;                   // 번호판: 부모 차량 id
  std::string imgref;                // 번호판: 카메라 크롭 경로 (없으면 "")
};

// 한 프레임 파싱 결과
struct Frame {
  double frame_w = 0, frame_h = 0;   // 갱신된 프레임 크기(px). scale 없으면 입력값 유지
  std::string plate_number;          // PlateNumber 텍스트(있으면), 없으면 ""
  std::vector<Object> objects;       // Human/Vehicle(유효중심) + LicensePlate 만
};

class Parser {
 public:
  // xml: 한 프레임 메타데이터. fw_prev/fh_prev: 직전 프레임 크기(이번에 scale 없으면 재사용).
  static Frame Parse(const std::string& xml, double fw_prev, double fh_prev) {
    Frame out;
    out.frame_w = fw_prev;
    out.frame_h = fh_prev;

    // 번호판 숫자 텍스트 (카메라가 주면 — 현재는 위치만 오지만 대비해 유지)
    size_t pn = xml.find("PlateNumber");
    if (pn != std::string::npos) out.plate_number = ExtractPlate(xml, pn);

    // 프레임 좌표계 크기 (ONVIF Transformation Scale): 폭 = 2/|scaleX|
    size_t sp = xml.find("<tt:Scale x=\"");
    if (sp != std::string::npos) {
      double sx = atof(xml.c_str() + sp + strlen("<tt:Scale x=\""));
      size_t yp = xml.find("y=\"", sp);
      double sy = (yp != std::string::npos) ? atof(xml.c_str() + yp + 3) : 0.0;
      if (std::fabs(sx) > 1e-9) out.frame_w = 2.0 / std::fabs(sx);
      if (std::fabs(sy) > 1e-9) out.frame_h = 2.0 / std::fabs(sy);
    }
    double fw = out.frame_w > 1.0 ? out.frame_w : 1.0;
    double fh = out.frame_h > 1.0 ? out.frame_h : 1.0;

    const std::string kObjTag = "<tt:Object ObjectId=\"";
    const std::string kCog = "<tt:CenterOfGravity x=\"";
    const std::string kBox = "<tt:BoundingBox ";

    size_t pos = 0;
    while (true) {
      size_t o = xml.find(kObjTag, pos);
      if (o == std::string::npos) break;

      size_t id_start = o + kObjTag.size();
      long id = atol(xml.c_str() + id_start);
      pos = id_start;  // 다음 반복 진행 보장 (무한루프 방지)

      size_t next_obj = xml.find(kObjTag, id_start);            // 이 객체의 끝 경계
      size_t obj_end = (next_obj == std::string::npos) ? xml.size() : next_obj;

      // 클래스 판별: 사람/차량/번호판 (Face/Head 등은 건너뜀). detect 필터는 호출자 몫.
      size_t hh = xml.find(">Human<", id_start);
      size_t vv = xml.find(">Vehicle<", id_start);
      size_t pl = xml.find(">LicensePlate<", id_start);
      bool is_human = (hh != std::string::npos && hh < obj_end);
      bool is_veh = (vv != std::string::npos && vv < obj_end);
      bool is_plate = (pl != std::string::npos && pl < obj_end);
      if (!is_human && !is_veh && !is_plate) continue;

      // 바운딩박스 (공통) → 정규화
      float nl = 0, nt = 0, nr = 0, nb = 0;
      size_t bx = xml.find(kBox, id_start);
      if (bx != std::string::npos && bx < obj_end) {
        double l = FindAttr(xml, bx, obj_end, "left=\"", 0);
        double t = FindAttr(xml, bx, obj_end, "top=\"", 0);
        double r = FindAttr(xml, bx, obj_end, "right=\"", 0);
        double b = FindAttr(xml, bx, obj_end, "bottom=\"", 0);
        nl = (float)std::min(1.0, std::max(0.0, l / fw));
        nt = (float)std::min(1.0, std::max(0.0, t / fh));
        nr = (float)std::min(1.0, std::max(0.0, r / fw));
        nb = (float)std::min(1.0, std::max(0.0, b / fh));
      }

      // 번호판: 부모(차량) id + 카메라 크롭(ImageRef) 경로 수집
      if (is_plate) {
        Object ob;
        ob.id = id;
        ob.cls = kPlate;
        ob.l = nl; ob.t = nt; ob.r = nr; ob.b = nb;
        size_t pa = xml.find("Parent=\"", o);
        if (pa != std::string::npos && pa < id_start + 64) ob.parent = atol(xml.c_str() + pa + 8);
        size_t ir = xml.find("<tt:ImageRef>", id_start);
        if (ir != std::string::npos && ir < obj_end) {
          size_t e = xml.find("</tt:ImageRef>", ir);
          if (e != std::string::npos) ob.imgref = xml.substr(ir + 13, e - (ir + 13));
        }
        out.objects.push_back(std::move(ob));
        continue;
      }

      // 사람/차량: CenterOfGravity(=중심)가 유효해야 추적 가능
      size_t c = xml.find(kCog, id_start);
      if (c == std::string::npos || c >= obj_end) continue;
      double cx = atof(xml.c_str() + c + kCog.size());
      size_t yq = xml.find("y=\"", c + kCog.size());
      if (yq == std::string::npos || yq >= obj_end) continue;
      double cy = atof(xml.c_str() + yq + 3);
      if (cx == 0.0 && cy == 0.0) continue;  // ImageRef 전용 등 무효 좌표

      Object ob;
      ob.id = id;
      ob.cls = is_veh ? kVehicle : kHuman;
      ob.has_center = true;
      ob.cx = (float)cx;
      ob.cy = (float)cy;
      ob.l = nl; ob.t = nt; ob.r = nr; ob.b = nb;
      out.objects.push_back(std::move(ob));
    }
    return out;
  }

 private:
  // [from, end) 범위에서 key(예: "left=\"") 뒤의 숫자 속성값을 읽는다. 없으면 def.
  static double FindAttr(const std::string& s, size_t from, size_t end, const char* key, double def) {
    size_t k = s.find(key, from);
    if (k == std::string::npos || k >= end) return def;
    return atof(s.c_str() + k + strlen(key));
  }
  // 문자열 앞뒤 공백/개행 제거
  static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
  }
  // "PlateNumber" 위치(from) 뒤에서 실제 번호판 글자를 뽑는다. 없으면 "".
  static std::string ExtractPlate(const std::string& xml, size_t from) {
    size_t p = from;
    for (int i = 0; i < 8; i++) {
      size_t gt = xml.find('>', p);
      if (gt == std::string::npos) break;
      size_t lt = xml.find('<', gt);
      if (lt == std::string::npos) break;
      std::string seg = Trim(xml.substr(gt + 1, lt - gt - 1));
      if (!seg.empty()) return seg;   // 태그 사이 실제 텍스트
      p = lt + 1;                     // 빈 조각(중첩 태그)이면 다음으로
    }
    return "";
  }
};

}  // namespace meta
