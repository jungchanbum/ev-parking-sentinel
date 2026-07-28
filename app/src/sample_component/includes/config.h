#pragma once

#include <cstdint>

// ============================================================================
// object_detect 설정값 단일 출처(single source of truth)
//   여기저기 흩어져 있던 경로/크기/개수를 한곳에 모음.
//   저장 폴더를 옮기거나 링버퍼를 키울 때 이 파일 한 줄만 고치면 전부 따라온다.
// ============================================================================
namespace cfg {

// 채널 수. 채널별 상태 배열은 [4]로 선언되어 있으므로 값이 바뀌면
// sample_component.h 의 배열 크기도 함께 맞춰야 한다(.cc 의 static_assert 가 감시).
constexpr int kChannels = 4;

// 저장 번호판 크롭 순환버퍼 칸 수 (cap_0.jpg ~ cap_99.jpg)
constexpr int kRingSize = 100;

// 카메라가 준 ImageRef 경로(/download/...)를 앱에서 읽을 때 붙이는 접두사.
//   ImageRef "/download/ch0/objectid_..jpg" → 실제 "/tmp/download/ch0/objectid_..jpg"
constexpr const char* kTmpPrefix = "/tmp";

// 앱이 결과물(크롭·스냅샷)을 저장하는 폴더 (실행 위치 기준 상대경로)
constexpr const char* kStorageDir = "../storage";

// ===== 번호판 저장 정책 =====
//   WiseAI 가 best-shot 을 1장 골라 주므로 앱은 선명도 측정/거부/비교 없이 그대로 저장.
//   (샤프니스 floor·best-shot 로직 제거됨 — 낮아도 전부 저장)

// [검증용] 원본 크롭을 cand_N.jpg 로 별도 덤프 (지금은 plate=원본이라 불필요 → false).
constexpr bool kDebugCandidates = false;
constexpr int  kCandRing = 24;            // 후보 이미지 보관 칸 수(cand_0..cand_23)

// 블러 완화(언샤프 마스크): 지금은 원본 그대로 저장(false).
//   OCR 이 흐려서 안 읽히면 true 로 → 저장 직전 엣지 강조. 과하면 노이즈 → Amount 튜닝.
constexpr bool   kEnhanceBlur   = false;
constexpr double kUnsharpAmount = 1.2;    // 강도 0.5~2.0
constexpr double kUnsharpSigma  = 1.5;    // 블러 반경 px

// OCR 최소 크롭 폭(px): 이보다 작으면(=차가 너무 멀면) 인식 스킵.
//   실측: 144·272px 크롭은 환각(conf 0.6~0.9 쓰레기), 328px+ 는 정상 판독.
constexpr int kMinOcrCropWidth = 300;

// ===== 번호판 숫자 인식(OCR) 모델 경로 =====
//   PC(ocr_lab)에서 검증한 tinyLPR 단독 (Multi-line 은 기여 0 실측으로 제거 — 경량화).
//   실행 위치(app/bin) 기준 상대경로 — res/ocr_models 에 패키징됨(CMakeLists 참고).
constexpr const char* kKrLprModel      = "../res/ocr_models/kr_lpr.tflite";
constexpr const char* kKrLprLabels     = "../res/ocr_models/kr_lpr_labels.txt";

// ===== 조도 정규화 (쌍라이트·역광 대응) =====
//   크롭의 평균 밝기를 재서 정상 범위 밖이면 감마 LUT 로 평균을 128 로 끌어와 재판독.
//   고정 감마(무조건 적용)는 실측 0 이었지만, 이건 "비정상일 때만" 발동하는 조건부 보정.
//   비용 ~1ms. conf 가 오를 때만 채택 — 멀쩡한 판은 건드리지 않는다.
constexpr bool kExposureNorm = true;
constexpr double kExposureLow  = 90.0;   // 평균 밝기 미만 = 너무 어두움(쌍라이트에 속은 AE)
constexpr double kExposureHigh = 170.0;  // 초과 = 과노출(번호판 반사)

// ===== 색-한글 일관성 검증 + 미세 지터 앙상블 =====
//   색: 노랑판(영업)만 아·바·사·자·배 허용, 그 외 판에선 금지 — 법정 규격.
//     흰판에서 "27아8257" 같은 환각을 포맷 모순으로 즉시 기각 (부스트가 아닌 필터라 무위험).
//     색 판정이 불확실(?)하면 제약을 걸지 않는다 (AWB 색쏠림 오판 방지).
//   지터: 이긴 크롭을 ±수 px 이동/스케일한 재프레이밍 앙상블 — tinyLPR 은 레터박스
//     정렬에 민감해 경계 걸친 글자가 2px 차이로 갈린다(멀티후보 원리의 미세 버전).
constexpr bool kColorRule = true;
constexpr bool kJitterEnsemble = true;   // conf<0.98 인 애매한 판에만 발동

// ===== 버스트 스태킹 (다중 프레임 평균 디노이즈) =====
//   같은 차의 버스트 크롭 N장을 고정 캔버스에 평균 — 노이즈는 프레임마다 무작위라
//   N장 평균 시 √N 배 감소, 글자는 모든 프레임에 동일해 보존 (천체사진 스태킹 원리).
//   추적 종료 시 1회 판독해 마지막 1표로 참여 (가공이므로 conf 0.95 캡).
// 실전 낙제(2026-07-28): 움직이는 차는 크롭 정렬이 안 맞아 평균이 죽이 됨 —
// stack 판독 전부 쓰레기(conf 0.53~0.80) + 차당 100~275ms 낭비 → 비활성.
constexpr bool kBurstStack = false;
constexpr int kStackMinFrames = 3;   // 최소 이 장수 이상 모였을 때만 평균 판독

// ===== 2단계 디노이즈 재판독 =====
//   광학 TTA 16종 중 유일하게 실측 이득이 있던 변형(fastNlMeans, PC +3%p).
//   이긴 후보 크롭에만 적용해 conf 가 오르면 채택 — 셔터 1/500 이후 오독의 주원인이
//   게인 노이즈로 바뀌어 효과 기대. 파라미터는 PC 검증값(h=5, template 7, search 15).
constexpr bool kDenoise2ndPass = true;

// ===== 버스트 샘플링 (부하 다이어트 2026-07-28) =====
//   웜업: 번호판 첫 감지 후 이 시간 동안은 버스트를 안 딴다 — 초반 크롭은 차가
//   제일 멀 때라 최저화질(작은 box, 낮은 conf)이면서 CPU 만 먹는 표였음(실측:
//   sample#1 이 거의 항상 그 차의 최저 conf). 상한도 12→6 으로 절반.
constexpr uint64_t kBurstWarmupMs = 1000;  // 첫 감지 후 웜업(ms)
constexpr uint64_t kBurstThrottleMs = 150; // 샘플 간 최소 간격(ms)
constexpr int kBurstMax = 6;               // 차당 버스트 상한 (good-shot 별도 +1표)

// ===== good-shot 품질 검문소 =====
//   선명도(라플라시안 분산). 주의: 이 지표는 노이즈에 오염됨 — 노이즈 심하면 정상이
//   100~366 으로 부풀고, BLC 로 화질이 깨끗해지면 정상이 12~31 까지 내려옴(실측).
//   따라서 "진짜 떡진 크롭"만 거르는 최소 문턱으로 운용.
constexpr double kGoodshotSharpMin = 10.0;

// ===== 번호판 최종확정(FINAL) 신뢰도 하한 =====
//   0.95 하향 실험은 롤백(2026-07-28) — 지터가 정답 0.95~0.96 을 오답으로 바꿔치기하는
//   사고가 커져서 원복. 가공은 "게이트 미달(=어차피 HOLD)일 때만" 개입하도록 별도 봉합.
constexpr double kFinalConfFloor = 0.96;
constexpr double kRescueConfCap  = 0.95;   // 가공 결과 conf 상한 — 게이트보다 항상 아래

// ===== 디버그 뷰어 로그 강조 (ANSI 색상) =====
//   뷰어가 이스케이프 코드를 못 그려서 "[1;92m" 같은 문자가 그대로 보이면 false 로 끄기.
constexpr bool kAnsiLogs = true;
constexpr const char* kAnsiFinal = "\x1b[1;92m";  // FINAL 확정 (밝은 초록, 굵게)
constexpr const char* kAnsiWarn  = "\x1b[1;93m";  // 저신뢰 FINAL (노랑 — conf<0.95 의심 구간)
constexpr const char* kAnsiInfo  = "\x1b[96m";    // OCR 결과 (시안)
constexpr const char* kAnsiDim   = "\x1b[90m";    // 버스트 샘플 (회색 — 참고용 잡음)
constexpr const char* kAnsiReset = "\x1b[0m";

// ===== 움직임 감지 튜닝 값 (MotionTracker 가 사용) =====
constexpr int kHistoryFrames = 6;         // 객체별로 보관할 최근 프레임 수
constexpr int kCompareBack = 5;           // 몇 프레임 전 좌표와 비교할지
constexpr double kMoveRatio = 0.03;       // 이동 > 좌표스케일*3% 이면 "움직임"
constexpr uint64_t kMovingHoldMs = 3000;  // 움직임 감지 후 이 시간(ms) 동안 moving 유지
constexpr uint64_t kStaleFrames = 15;     // 이 프레임 이상 안 보이면 추적 삭제
// 번호판 확정 만료의 벽시계 백업(ms). 영상이 끝나 그 채널 메타데이터가 끊기면
// 프레임 카운터(tick)가 멈춰 위 조건이 영원히 안 참 → 마지막 차가 팬딩에 갇힘.
// 어느 채널 프레임에서든 전 채널을 벽시계로도 점검한다 (primary 없으면 ×3 유예).
constexpr uint64_t kStaleMs = 3000;

}  // namespace cfg
