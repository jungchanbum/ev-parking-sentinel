# 카메라 골든 파라미터 — PNM-C16083RVQ (번호판 채널)

2026-07-29 17:45 실측 확정본. 이 세팅에서 야간 오확정 0 / 발급률 96~100% 다수 런 검증.
웹 UI: 설정 → 비디오/오디오 → 카메라 설정 → **사용자지정 프리셋 1** (번호판 보는 센서 채널에 적용).

> 재부팅 후 채널 매핑이 바뀔 수 있음(실측: ch3→ch2). 세팅은 "그 센서"에 붙어 있으니
> 프리뷰를 보고 번호판 화각 채널인지 확인할 것.

| 탭 | 항목 | 값 | 비고 |
|---|---|---|---|
| 센서 | 센서 모드 | 30 fps | |
| SSDR | 사용 | ✔ | |
| | 레벨 | 16 | |
| | D-Range | 넓음 | |
| 노출 보정 | 밝기 | 70 | |
| | 최소 셔터 | 1/600 | 모니터 재생 모션블러 방지 핵심 |
| | 최대 셔터 | 1/12000 | |
| | 사용자 선호 셔터 | 1/600 (AI 셔터 제어 OFF) | |
| | 플리커 방지 | 사용 안 함 | |
| | SSNR | 사용, 레벨 8 | |
| | AGC | 중간 | |
| 역광 보정 | 모드 | BLC, 레벨 70 | |
| | BLC 영역 | 화면 좌측 블록 | 42주0120 통과 경로(어두운 구간) 커버 — lum 62→100+ 상승 실측 |
| | 동작 스케줄 | 사용 안 함 | |
| 주야간 모드 | 모드 | **컬러 고정** | 자동이면 야간에 흑백+IR로 널뛰어 색판정·노출 붕괴 (실측 사고) |
| IR | 모드/레벨 | 자동 1 / 100 | 컬러 고정이라 실제 점등 안 함 |
| 스페셜 | 윤곽 조정 | 사용, 레벨 12 | |
| | 감마 | 0.55 | |
| | 대비 / 컬러 레벨 | 50 / 50 | |
| | 안개 제거 / LDC | 사용 안 함 / 사용 안 함 | |

## 운영 수칙 (실측 교훈)

- **cap 설치는 테스트 영상 정지 상태에서, 가급적 카메라 재부팅 직후에.**
  장시간 영상 재생 후엔 임시공간 고갈로 설치가 "패키지 오류(OpenSDKError 105)"로 튕김.
- 주야간 "자동" 금지 — 모니터 리그에선 방 조도 때문에 야간 모드로 오판해 흑백 전환됨.
- 원본 녹화: `C:\Users\3-28\Documents\Bandicam\bandicam 2026-07-29 17-44-45-249.mp4`

## WiseAI 분석 설정 (2026-08-06 확정 — 전 채널 동일)

작은 번호판(모형·원거리)이 검출기에 도달하기 전에 잘려나가던 원인이
`minimumObjectSizeInPixels`(기본 46×46) 하한이었음 — 번호판은 납작해서 세로가
하한 미달로 필터링됨(실측: 하향 후 모형 판 감지 확보). 카메라 초기화·WiseAI
재설치 시 아래로 복원:

| 항목 | 값 | 비고 |
|---|---|---|
| sensitivity | 90 | 100은 노이즈 오검출 증가 → 90으로 타협(실측) |
| minimumObjectSizeInPixels | **24×24** | 기본 46×46이 작은 판을 걸러냄. 12까지 가능하나 24가 균형점 |
| maximumObjectSizeInPixels | 2592×1520 | 근접 대형 객체 컷 해제 |
| minimumObjectSize(정규화) | 0×0 | |
| maximumObjectSize(정규화) | 99×99 | |

복원 명령 (채널 0~3 전부):

```bash
for c in 0 1 2 3; do curl -s --digest -u admin:'PW' -X PUT -H "Content-Type: application/json" \
  -d "{\"channel\":$c,\"sensitivity\":90,\"minimumObjectSize\":{\"width\":0,\"height\":0},\"maximumObjectSize\":{\"width\":99,\"height\":99},\"minimumObjectSizeInPixels\":{\"width\":24,\"height\":24},\"maximumObjectSizeInPixels\":{\"width\":2592,\"height\":1520}}" \
  "http://192.168.0.5/opensdk/WiseAI/configuration/commonanalyticssettings"; done
```

검증: 같은 URL GET — PUT "Success" 라도 무반영인 항목(imageQuality 45 고정 등)이
있으므로 반드시 GET 재확인. WiseAI 설정은 카메라 재부팅에는 유지됨.
