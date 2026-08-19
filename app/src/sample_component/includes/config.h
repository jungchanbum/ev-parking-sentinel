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
// [FINAL 증거 갤러리 08-13] ★FINAL 에 실제 사용된 이미지 전용 링 (final_<n>.jpg).
//   내부 링(cap_)은 OCR 입력용 전량 저장이고, UI 갤러리·evidence URL 은 이것만 본다.
constexpr int kFinalRing = 24;

// 카메라가 준 ImageRef 경로(/download/...)를 앱에서 읽을 때 붙이는 접두사.
//   ImageRef "/download/ch0/objectid_..jpg" → 실제 "/tmp/download/ch0/objectid_..jpg"
constexpr const char* kTmpPrefix = "/tmp";

// 앱이 결과물(크롭·스냅샷)을 저장하는 폴더 (실행 위치 기준 상대경로)
constexpr const char* kStorageDir = "../storage";

// ===== 주차구역(ParkingZone) 판정 =====
//   Pi 가 4점 구역 등록 → 차가 그 구역 면적 kParkOverlapFrac 이상 겹치고 정지한 채
//   kParkDwellMs 유지 → "주차". 점유차가 빠지면 kParkLeaveGraceMs 후 디폴트 리셋.
constexpr double kParkOverlapFrac = 0.65;   // bbox 면적의 65% 이상 구역 안 (진입 판정 겸용)
constexpr uint64_t kParkDwellMs = 2000;     // 정지+겹침 2초 유지 → 주차 확정
constexpr uint64_t kParkLeaveGraceMs = 2000;  // 부재 이만큼 지속 → "부재 의심" (출차 확정은
                                              //   스냅샷 육안검증 3연속 빈칸이 내린다)
constexpr const char* kParkFile = "../storage/parking_zones.txt";  // 구역 영속화
// FINAL 이 주차확정(3초)보다 먼저 끝나면 배정이 비켜간다 (진입하며 읽히고 그다음 주차).
//   1순위는 명찰 직결(번호판 parent 차량 oid)이고, 이건 추적이 끊겼을 때의 최후 폴백 —
//   배정 즉시 1회용 소모되며, 창을 짧게 둬서 남의 번호가 꽂힐 확률을 줄인다.
constexpr uint64_t kParkPlateBacklogMs = 30000;   // 최근 FINAL 소급 배정 윈도우 (30초)

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

// OCR 최소 크롭 폭·세로 하한(px): 이보다 작으면 인식 스킵.
//   08-03 재조정: 수동 초점 도입 후 작은 크롭도 선명해져 240x160 크롭에서
//   "36라7833", 384x168 에서 "15노1199" 가 또렷이 읽힘(실측 확인). 기존 300x176
//   (07-29, 소프트 크롭 기준)은 이제 과해서 읽을 수 있는 판을 버림 → 220x150 으로 완화.
//   여전히 144px 이하 납작 크롭은 글자가 뭉개져 제외(미검증). 낮춘 만큼 저품질 판독이
//   늘 수 있으나 0.95 게이트·0.90 버스트 게이트가 오확정을 막음(등록차 DB 회수만 주의).
// 08-03 최종: 모델 입력이 96px 높이/192px 폭이라, 그보다 작은 크롭은 upscale 이라
//   정보가 안 늘어남 = 물리적 하한. 실측: 360x120 sharp573 에서 "27머8257" 또렷이 읽힘.
// 08-04 재완화: 주차 리그에서 카메라 ImageRef 크롭이 88~176px 폭으로 와서 전량 스킵
//   → 같은 판을 버스트(123x110)는 conf 0.98 로 잘 읽음(실측). upscale 구간이어도
//   시도는 하게 풀고, 오확정은 0.95 게이트·버스트 캡·DB 그물이 막는다.
constexpr int kMinOcrCropWidth = 64;
constexpr int kMinOcrCropHeight = 44;   // 08-12 완화: 멀리/작은 토이판 good-shot 이 120x48 로 와서
                                        //   높이 60 에 걸려 전량 스킵(실측) → 버스트는 같은 48px 를
                                        //   conf 0.9+ 로 읽음. "조금만 여유롭게"(사용자) — 44 로.
                                        //   더 작아지면 환각↑ 이니 여기가 실용 하한.

// good-shot 블러 하한 (라플라시안 분산). good-shot 척도: 정상 100~366·물렁 10~13.
//   이 미만은 최종 OCR 스킵 — 심한 블러가 환각 번호를 뱉는 것 차단 (08-11 사용자 요청).
constexpr double kGoodshotMinSharp = 40.0;

// [명부 낀 버스트 구제 08-14] 버스트 환각 게이트(0.90) 미달이어도, 이 값 이상이면서
//   판독 텍스트가 명부와 "유일" 근사매칭(≤2글자)이면 교정 텍스트로 표를 인정한다.
//   근거 실측: 경계 품질 크롭(201×48, color ?)에서 "42노499X" conf 0.85~0.89 가 20초간
//   전량 폐기돼 FINAL 이 22.5초 걸림 — 명부가 문맥이 되면 2~3초면 끝날 판이었다.
//   미등록 번호엔 영향 0 (기존 0.90 게이트 그대로).
constexpr double kBurstDbAssistMin = 0.85;

// 크롭 좌우 반전 보정: true 면 판독 직전 크롭을 수평으로 뒤집는다.
//   근본 원인은 카메라(채널/센서 플립)지만, 카메라에서 못 끄는 경우의 코드 보정.
//   2026-08-03 실측: 재부팅 후 번호판 영상이 좌우 반전된 채널로 잡혀 OCR 전량 오독
//   (cap_ 크롭이 거울상). 정상(비반전) 채널로 돌아가면 반드시 false 로.
constexpr bool kFlipCropH = true;    // 2026-08-11 재확정: 원본 스냅샷에서 푸조 로고·번호판이
                                     //   좌우 반전 → 카메라 센서가 거울상. 크롭은 뒤집어야 정방향.
                                     //   (낮에 잠깐 false 로 뒤집었다가 오히려 전량 거울상 OCR →
                                     //    복구. 스냅샷 로고 반전이 명백한 근거.)

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

// ===== 구조대 체인(샤프닝·디노이즈·채널) 마스터 스위치 =====
//   부하 다이어트(07-29): 건당 100~460ms 를 먹는 최대 낭비 구간인데, 캡 0.94 라
//   단독으론 FINAL 을 못 만들고 산출물 대부분이 쓰레기 실측 → 완전 오프.
//   이 구간이 살리던 저화질 판은 DB 최근접 매칭 레이어가 이어받는다.
constexpr bool kRescueChain = false;

// ===== 디블러(언샤프) — 흐린(디포커스) 크롭 전용 =====
//   구조체인과 별개의 가벼운 언샤프 1회(~2ms, 디노이즈 없음). conf<0.95 인 흐린 판독에만
//   발동해 글자 경계를 세운다. 캡 0.94 라 단독 FINAL 불가 — DB 그물·투표 보조용.
//   08-03: 초점 흐림 크롭(sharp<300)에서 판독 개선 시도. 오독 늘면 false 로.
constexpr bool kDeblurSoft = true;

// ===== 2단계 디노이즈 재판독 =====
//   광학 TTA 16종 중 유일하게 실측 이득이 있던 변형(fastNlMeans, PC +3%p).
//   이긴 후보 크롭에만 적용해 conf 가 오르면 채택 — 셔터 1/500 이후 오독의 주원인이
//   게인 노이즈로 바뀌어 효과 기대. 파라미터는 PC 검증값(h=5, template 7, search 15).
constexpr bool kDenoise2ndPass = true;

// ===== tinyLPR 인퍼런스 스레드 수 =====
//   2 실험 낙제(2026-07-28 17시): 첫 판독부터 178ms 로 오히려 느려지고 직후 앱이
//   얼어붙음 — 스레드풀 스핀 대기가 2코어를 독점해 다른 스레드가 전부 굶은 것으로
//   추정. 이 SoC/cgroup 조합에선 1 이 정답.
constexpr int kTfliteThreads = 1;

// ===== 버스트 샘플링 (부하 다이어트 2026-07-28) =====
//   상한 3 + 웜업 1초(첫 감지 직후 최저화질 구간 스킵). 웜업 1차 시도는 낙제였지만
//   그건 즉시확정 도입 전 — 지금은 good-shot 0.99+ 가 대부분을 즉석에서 끝내
//   버스트 의존도가 낮아져 재도전(사용자 결정). HOLD 급증 시 웜업만 원복할 것.
constexpr uint64_t kBurstWarmupMs = 1000;  // 첫 감지 후 이 시간은 버스트 안 딴다
constexpr uint64_t kBurstThrottleMs = 150; // 샘플 간 최소 간격(ms)
constexpr int kBurstMax = 3;               // 차당 버스트 상한 (good-shot 별도 +1표)

// ===== 등록차량 DB 최근접 매칭 =====
//   HOLD 회수 + FINAL 교정 레이어 (plate_db.h). 파일 없으면 자동 오프(순수 인식 모드).
//   db-rescue 최소 conf: 쓰레기 판독(0.5~0.8)이 우연히 1글자 매칭돼 승격되는 것 방지.
constexpr bool kPlateDb = true;
constexpr const char* kPlateDbFile = "../res/ocr_models/registered_plates.txt";
//   07-29 실측 하향(0.85/0.90 → 0.80/0.85): 42주0129(0.89)·36보7833(0.85)이 하한에
//   아깝게 걸려 억류됐는데, 같은 런의 진짜 쓰레기들은 전부 2글자+ 어긋나 하한을
//   내려도 오회수 0건이었음. 유일매칭+등록부 제약이 이미 강한 그물이라 conf 는 보조핀.
constexpr double kDbRescueExactMin = 0.80;  // 정확 일치 회수 최소 conf
constexpr double kDbRescueEd1Min   = 0.85;  // 1글자 교정 회수 최소 conf

// ===== good-shot 품질 검문소 =====
//   선명도(라플라시안 분산). 주의: 이 지표는 노이즈에 오염됨 — 노이즈 심하면 정상이
//   100~366 으로 부풀고, BLC 로 화질이 깨끗해지면 정상이 12~31 까지 내려옴(실측).
//   따라서 "진짜 떡진 크롭"만 거르는 최소 문턱으로 운용.
constexpr double kGoodshotSharpMin = 10.0;

// ===== 번호판 최종확정(FINAL) 신뢰도 하한 =====
//   0.95 재개방(2026-07-28 오후, 사용자 결정): 첫 0.95 실험을 망친 범인(지터가
//   FINAL 자격 판독을 바꿔치기)은 봉합 완료라 재도전 조건이 다름. 캡은 0.94 로
//   내려 "가공은 단독으로 게이트를 못 넘는다" 원칙 유지.
constexpr double kFinalConfFloor = 0.95;
constexpr double kRescueConfCap  = 0.94;   // 가공 결과 conf 상한 — 게이트보다 항상 아래

// ===== 후보 캐스케이드 (연산 절감) =====
//   good-shot 판독 시 후보 크롭을 순서대로(orig→box0m0→ctr0→ctr1) 읽다가, 유효(포맷+색)
//   판독이 이 값 이상이면 즉시 멈춘다 — 나머지 후보·지터·조도 전부 스킵. 깨끗한 판은
//   4회 OCR → 1~2회로 줆. 봉인 이력(07-29): 0.99 조기종료는 orig 0.99 오독을 ctr 정답이
//   못 뒤집는 사고가 있었음 → 재도전 조건: 수동초점+정상방향으로 크롭 품질이 오른 지금,
//   문턱을 실측으로 검증. 하나라도 오확정 나오면 0.995 로 조이거나 kCascade=false 로 끈다.
constexpr bool   kCascade     = false;  // 롤백(08-03): 0.99 조기종료가 오독 증가시킴
constexpr double kCascadeExit = 0.99;   // (미사용) 이 conf 이상이면 남은 후보 생략

// box0m0 후보(Otsu+윤곽선). 우승 횟수는 적지만 소프트 크롭을 건지는 rescue 역할이라
//   끄면 HOLD 가 늘어 되돌림(08-03 실측). 우승통계만 보고 제거하면 안 됨.
constexpr bool   kUseBoxCand  = true;

// ===== good-shot 즉시확정 =====
//   good-shot 원본 판독(가공 아님)이 이 값 이상이면 개표를 기다리지 않고 그 자리에서
//   ★FINAL + 그 차의 버스트·재판독 전부 중단(CPU 절약). 전 로그 실측: 0.99·1.00 은
//   틀린 사례 0회, 0.98 은 오독 실례 있음(09서2646) → 0.985 가 안전 하한
//   (표시 0.99 = 내부 0.985~0.994 라 0.99·1.00 을 모두 포섭).
constexpr double kInstantFinalConf = 0.985;
// 즉시확정 충돌 검사 문턱: 투표함의 반박 텍스트가 이 값 이상일 때만 instant 보류.
//   0.95 로 두면 버스트 잡음(0.95~0.97 오독)이 1.00 정답의 instant 를 막고, 이어서
//   접전 룰이 그 정답을 HOLD 로 눌러버리는 사고 발생(09서2645 1.00 인질 사건, 07-29).
constexpr double kInstantConflictConf = 0.98;

// ===== EV 실조회 (무공해차 통합누리집 ev.or.kr) =====
//   미등록 번호판의 전기차 여부를 카메라가 직접 조회 (등록차는 DB 플래그 우선).
//   판 색으로는 EV 를 판정하지 않는다: 파란판만 전기차가 아니다 — 법인 전기차는
//   연두색(친환경 법인판)이라 "파랑=EV" 는 법인차를 통째로 놓친다 (07-30 확정).
//   따라서 EV 의 정답은 오직 ① 등록 DB 플래그 ② ev.or.kr 실조회 둘뿐.
//   끄면 미등록차는 "판정불가"로 보류 (인터넷 없는 현장 — 색 추정 안 함).
constexpr bool kEvLiveLookup = true;
//   조회 엔드포인트 — 기관이 바뀌면 여기만 고친다 (앱 코드에 URL 하드코딩 금지).
constexpr const char* kEvLookupUrl =
    "https://ev.or.kr/nportal/buySupprt/selectNonpolluCheck.ajax";

// ===== 디버그 뷰어 로그 강조 (ANSI 색상) =====
//   뷰어가 이스케이프 코드를 못 그려서 "[1;92m" 같은 문자가 그대로 보이면 false 로 끄기.
constexpr bool kAnsiLogs = true;
constexpr const char* kAnsiFinal = "\x1b[1;92m";  // FINAL 확정 (밝은 초록, 굵게)
constexpr const char* kAnsiWarn  = "\x1b[1;93m";  // 저신뢰 FINAL (노랑 — conf<0.95 의심 구간)
constexpr const char* kAnsiInfo  = "\x1b[96m";    // OCR 결과 (시안)
constexpr const char* kAnsiDim   = "\x1b[90m";    // 버스트 샘플 (회색 — 참고용 잡음)
constexpr const char* kAnsiEv    = "\x1b[1;95m";  // EV 판정 — 전기차 (밝은 자홍, 굵게)
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
