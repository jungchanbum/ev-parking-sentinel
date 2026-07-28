#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <tensorflow/lite/c/c_api.h>

// ============================================================================
// TfliteModel — TFLite C API(extern "C", ABI 안정) 위의 얇은 RAII 래퍼.
//   C API를 쓰는 이유: 카메라 툴체인(gcc12.1)과 이 라이브러리를 빌드한 컴파일러가
//   달라도, extern "C" 경계는 C++ STL ABI 문제(이름 맹글링·컨테이너 레이아웃)에서
//   자유롭다. 입력은 항상 "그레이스케일 H×W, row-major float32(0~1)"로 통일—
//   채널=1이면 NHWC/NCHW 구분이 메모리상 동일해지므로 이 앱에선 신경 쓸 필요 없다.
// ============================================================================
class TfliteModel {
 public:
  bool Load(const std::string& model_path, int num_threads = 1) {
    model_ = TfLiteModelCreateFromFile(model_path.c_str());
    if (!model_) return false;
    TfLiteInterpreterOptions* opts = TfLiteInterpreterOptionsCreate();
    TfLiteInterpreterOptionsSetNumThreads(opts, num_threads);
    interp_ = TfLiteInterpreterCreate(model_, opts);
    TfLiteInterpreterOptionsDelete(opts);  // 옵션은 생성 시 복사됨 — 즉시 해제 가능
    if (!interp_) return false;
    return TfLiteInterpreterAllocateTensors(interp_) == kTfLiteOk;
  }

  ~TfliteModel() {
    if (interp_) TfLiteInterpreterDelete(interp_);
    if (model_) TfLiteModelDelete(model_);
  }
  TfliteModel() = default;
  TfliteModel(const TfliteModel&) = delete;
  TfliteModel& operator=(const TfliteModel&) = delete;

  // 입력 텐서의 (height, width) — 전처리(리사이즈 크기) 결정에 필요.
  void InputHW(int* h, int* w) const {
    const TfLiteTensor* t = TfLiteInterpreterGetInputTensor(interp_, 0);
    int nd = TfLiteTensorNumDims(t);
    // NHWC([1,H,W,1])·NCHW([1,1,H,W]) 둘 다 지원: 채널=1 인 두 중간 차원이 H,W.
    *h = TfLiteTensorDim(t, 1);
    *w = TfLiteTensorDim(t, 2);
    if (nd == 4 && TfLiteTensorDim(t, 1) == 1) {  // NCHW: [1,1,H,W]
      *h = TfLiteTensorDim(t, 2);
      *w = TfLiteTensorDim(t, 3);
    }
  }

  // gray: row-major H*W float32(0~1) 연속 버퍼. out: [T, num_classes] 형태로 반환(T*num_classes 크기).
  bool Run(const float* gray, size_t n_floats, std::vector<float>* out, int* T, int* num_classes) {
    TfLiteTensor* in = TfLiteInterpreterGetInputTensor(interp_, 0);
    if (TfLiteTensorCopyFromBuffer(in, gray, n_floats * sizeof(float)) != kTfLiteOk) return false;
    if (TfLiteInterpreterInvoke(interp_) != kTfLiteOk) return false;
    const TfLiteTensor* ot = TfLiteInterpreterGetOutputTensor(interp_, 0);
    int nd = TfLiteTensorNumDims(ot);
    int classes = TfLiteTensorDim(ot, nd - 1);
    int total = 1;
    for (int i = 0; i < nd; i++) total *= TfLiteTensorDim(ot, i);
    out->resize(total);
    if (TfLiteTensorCopyToBuffer(ot, out->data(), total * sizeof(float)) != kTfLiteOk) return false;
    *num_classes = classes;
    *T = total / classes;
    return true;
  }

 private:
  TfLiteModel* model_ = nullptr;
  TfLiteInterpreter* interp_ = nullptr;
};
