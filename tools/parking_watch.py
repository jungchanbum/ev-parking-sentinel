#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# ============================================================================
# parking_watch.py — 주차 이벤트 워처 (PC/라파 실험용)
#
#   구조: monitordiff(벨) 상시 연결 → object_detect 벨 라인이 변하면
#         /parking_status(XML) 열람 → 칸별로 직전 상태와 비교해 이벤트 출력.
#
#   출력 이벤트:
#     🚗 주차   — 칸이 점유됨 (번호 아직이면 "판독중")
#     🔄 갱신   — 점유 중 내용 변화 (번호 확정 / EV 판정 / 위반 확정)
#     🏳️ 출차   — 칸이 비워짐
#
#   실행:  python3 parking_watch.py        (requests 없으면: pip3 install requests)
# ============================================================================
import re
import sys
import time
import xml.etree.ElementTree as ET
from datetime import datetime, timedelta

import requests
from requests.auth import HTTPDigestAuth

CAM = "http://192.168.0.5"
AUTH = HTTPDigestAuth("admin", "5hanwha!")
# 벨: ParkingOccupied(주차/출차·칸별) + ParkingViolation(위반) — 어느 줄이든 변하면 XML 열람
BELL = re.compile(r"Channel\.(\d+)\.OpenSDK\.object_detect\.Parking(Occupied|Violation)")
SAFETY_POLL_S = 10          # 벨을 놓쳐도 이 주기로 XML 재확인 (안전망)
DEBOUNCE_S = 0.4            # 펄스(라인 2개 연속) 를 XML 1회 열람으로 합침

prev = {}                   # 칸 id → 마지막 상태 dict


def ts():
    return datetime.now().strftime("%H:%M:%S")


def fetch_status():
    """XML 서류철 열람 → {칸id: 상태dict}"""
    r = requests.get(f"{CAM}/opensdk/object_detect/parking_status",
                     auth=AUTH, timeout=5)
    r.encoding = "utf-8"
    spaces = {}
    for sp in ET.fromstring(r.text).findall("space"):
        ago = int(sp.findtext("parked_ms_ago") or -1)
        ev_el = sp.find("ev")
        spaces[sp.get("id")] = {
            "id": sp.get("id"),
            "ch": sp.get("channel"),
            "occupied": sp.findtext("occupied") == "true",
            "plate": (sp.findtext("plate") or "").strip(),
            "ev": ev_el.text if ev_el is not None and ev_el.text else "unknown",
            "ev_src": ev_el.get("source", "") if ev_el is not None else "",
            "violation": sp.findtext("violation") == "true",
            "parked_at": (datetime.now() - timedelta(milliseconds=ago))
                         if ago >= 0 else None,
            "evidence": sp.findtext("evidence") or "",
        }
    return spaces


def describe(d):
    """칸 상태 한 줄 요약"""
    plate = d["plate"] if d["plate"] else "(판독중…)"
    ev = {"yes": "⚡전기차", "no": "내연차", "unknown": "EV판정중"}[d["ev"]]
    ok = "⛔위반(비EV)" if d["violation"] else ("✅정상" if d["ev"] == "yes" else "⏳판정대기")
    when = d["parked_at"].strftime("%H:%M:%S") if d["parked_at"] else "-"
    src = f" ({d['ev_src']})" if d["ev_src"] else ""
    return (f"칸={d['id']} ch{d['ch']} | 번호={plate} | 주차시각={when} | "
            f"{ev}{src} | {ok}")


def sig(d):
    """비교용 핵심 필드만 — parked_at 은 ms 역산 오차로 매번 흔들려서 제외 (갱신 도배 방지)"""
    return (d["occupied"], d["plate"], d["ev"], d["ev_src"], d["violation"])


def diff_and_print():
    """XML 열람 → 직전과 비교 → 칸별 이벤트 print"""
    global prev
    try:
        cur = fetch_status()
    except Exception as e:
        print(f"[{ts()}] ⚠️ XML 열람 실패: {e}")
        return
    for sid, d in cur.items():
        p = prev.get(sid)
        was = p["occupied"] if p else False
        if d["occupied"] and not was:
            print(f"[{ts()}] 🚗 주차   {describe(d)}")
            if d["evidence"]:
                print(f"           증거사진: {CAM}{d['evidence']}")
        elif d["occupied"] and was and sig(p) != sig(d):
            print(f"[{ts()}] 🔄 갱신   {describe(d)}")
        elif not d["occupied"] and was:
            plate = p["plate"] or "(미판독)"
            print(f"[{ts()}] 🏳️ 출차   칸={sid} | 나간 차={plate}")
    for sid in set(prev) - set(cur):        # 구역 자체가 삭제된 경우
        print(f"[{ts()}] 🗑 구역 삭제됨: {sid}")
    prev = cur


def main():
    print(f"[{ts()}] 주차 워처 시작 — 카메라 {CAM}, 채널 0~3 감시")
    diff_and_print()                        # 시작 시 현재 상태 1회
    last_read = time.time()
    while True:
        try:
            r = requests.get(
                f"{CAM}/stw-cgi/eventstatus.cgi"
                "?msubmenu=eventstatus&action=monitordiff",
                auth=AUTH, stream=True, timeout=(5, SAFETY_POLL_S))
            pending = False
            for raw in r.iter_lines():
                line = raw.decode("utf-8", "ignore")
                if BELL.search(line):
                    pending = True          # 벨 울림 — 잠깐 모았다가 1회 열람
                if pending and time.time() - last_read >= DEBOUNCE_S:
                    diff_and_print()
                    last_read = time.time()
                    pending = False
                if time.time() - last_read >= SAFETY_POLL_S:  # 안전망 폴링
                    diff_and_print()
                    last_read = time.time()
            if pending:                     # 스트림이 끊기기 직전 벨 처리
                diff_and_print()
                last_read = time.time()
        except requests.exceptions.ReadTimeout:
            # 정상 동작 — 스트림이 SAFETY_POLL_S 초 조용하면 안전망 폴링 겸 조용히 재접속
            diff_and_print()
            last_read = time.time()
        except KeyboardInterrupt:
            print("\n종료")
            sys.exit(0)
        except requests.exceptions.ConnectionError:
            # 진짜 끊김(카메라 재부팅 등)만 표시
            print(f"[{ts()}] ⚠️ 카메라 연결 끊김 — 3초 후 재접속")
            time.sleep(3)
        except Exception as e:
            print(f"[{ts()}] ⚠️ 오류({type(e).__name__}) — 3초 후 재접속")
            time.sleep(3)


if __name__ == "__main__":
    main()
