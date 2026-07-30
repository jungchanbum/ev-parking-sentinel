#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ev_lookup — 번호판 → 전기차(1종 저공해) 판정.

무공해차 통합누리집(ev.or.kr)의 '내차 저공해 확인' 조회를 그대로 호출한다.
로그인·캡차·apiKey 없음. 차량365 백엔드가 차량 등록 시점에 1회 부르는 용도
(공식 오픈API가 아니므로 대량·반복 호출 금지 — 등록당 1회면 충분).

판정 규칙 (2026-07-30 실측):
  data == null            → 저공해차 아님        → ev=false
  CAR_TYPE_NM == "1종"    → 전기·수소 (파란판)   → ev=true
  CAR_TYPE_NM == "2종"    → 하이브리드           → ev=false
  실측 양성: 50구5529 → {"MAKR_NM":"기아자동차(주)","CAR_NM":"EV6",
                         "USEFUELNM":"전기","CAR_TYPE_NM":"1종"}

사용:
  python ev_lookup.py 50구5529
  python ev_lookup.py 50구5529 --update ../app/res/ocr_models/registered_plates.txt
    → 판정 결과대로 해당 번호 줄에 ",ev" 플래그를 넣거나 뺀다 (없는 번호면 추가).
"""
import argparse
import json
import sys
import urllib.parse
import urllib.request

URL = "https://ev.or.kr/nportal/buySupprt/selectNonpolluCheck.ajax"

if sys.stdout.encoding and sys.stdout.encoding.lower() != "utf-8":
    sys.stdout.reconfigure(encoding="utf-8")  # Windows 콘솔(CP949) 한글 깨짐 방지


def lookup(plate: str, timeout: float = 15.0) -> dict:
    """조회 1회. 반환: {plate, ev, type, fuel, model, maker} (미저공해면 type 이하 None)."""
    body = urllib.parse.urlencode({"selectCarNum": "1", "searchWord": plate}).encode()
    req = urllib.request.Request(
        URL, data=body,
        headers={"User-Agent": "Mozilla/5.0", "X-Requested-With": "XMLHttpRequest"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        data = json.load(r).get("data")
    if not data:
        return {"plate": plate, "ev": False,
                "type": None, "fuel": None, "model": None, "maker": None}
    return {"plate": plate,
            "ev": data.get("CAR_TYPE_NM") == "1종",
            "type": data.get("CAR_TYPE_NM"),
            "fuel": data.get("USEFUELNM"),
            "model": data.get("CAR_NM"),
            "maker": data.get("MAKR_NM")}


def update_db(path: str, plate: str, ev: bool) -> str:
    """registered_plates.txt 의 해당 번호 줄에 ev 플래그를 반영. 반환: 수행한 동작."""
    with open(path, encoding="utf-8") as f:
        lines = f.read().splitlines()
    out, hit = [], False
    for ln in lines:
        body = ln.strip()
        if body and not body.startswith("#") and body.split(",")[0] == plate:
            hit = True
            ln = plate + (",ev" if ev else "")
        out.append(ln)
    if not hit:
        out.append(plate + (",ev" if ev else ""))
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    return ("updated" if hit else "added") + (" +ev" if ev else "")


def main():
    ap = argparse.ArgumentParser(description="번호판 → 전기차(1종 저공해) 판정")
    ap.add_argument("plate", help="차량등록번호 (예: 50구5529)")
    ap.add_argument("--update", metavar="PLATES_TXT",
                    help="registered_plates.txt 경로 — 판정 결과를 파일에 반영")
    args = ap.parse_args()

    try:
        res = lookup(args.plate)
    except Exception as e:
        print(json.dumps({"plate": args.plate, "error": str(e)}, ensure_ascii=False))
        sys.exit(2)

    if args.update:
        res["db"] = update_db(args.update, args.plate, res["ev"])
    print(json.dumps(res, ensure_ascii=False))
    sys.exit(0 if res["ev"] else 1)


if __name__ == "__main__":
    main()
