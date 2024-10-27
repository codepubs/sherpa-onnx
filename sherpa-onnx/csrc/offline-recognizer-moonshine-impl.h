// sherpa-onnx/csrc/offline-recognizer-moonshine-impl.h
//
// Copyright (c)  2024  Xiaomi Corporation

#ifndef SHERPA_ONNX_CSRC_OFFLINE_RECOGNIZER_MOONSHINE_IMPL_H_
#define SHERPA_ONNX_CSRC_OFFLINE_RECOGNIZER_MOONSHINE_IMPL_H_

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if __ANDROID_API__ >= 9
#include "android/asset_manager.h"
#include "android/asset_manager_jni.h"
#endif

#include "sherpa-onnx/csrc/offline-model-config.h"
#include "sherpa-onnx/csrc/offline-moonshine-decoder.h"
#include "sherpa-onnx/csrc/offline-moonshine-greedy-search-decoder.h"
#include "sherpa-onnx/csrc/offline-moonshine-model.h"
#include "sherpa-onnx/csrc/offline-recognizer-impl.h"
#include "sherpa-onnx/csrc/offline-recognizer.h"
#include "sherpa-onnx/csrc/symbol-table.h"
#include "sherpa-onnx/csrc/transpose.h"

namespace sherpa_onnx {

static OfflineRecognitionResult Convert(
    const OfflineMoonshineDecoderResult &src, const SymbolTable &sym_table) {
  OfflineRecognitionResult r;
  r.tokens.reserve(src.tokens.size());

  std::string text;
  for (auto i : src.tokens) {
    if (!sym_table.Contains(i)) {
      continue;
    }

    const auto &s = sym_table[i];
    text += s;
    r.tokens.push_back(s);
  }

  r.text = text;

  return r;
}

class OfflineRecognizerMoonshineImpl : public OfflineRecognizerImpl {
 public:
  explicit OfflineRecognizerMoonshineImpl(const OfflineRecognizerConfig &config)
      : OfflineRecognizerImpl(config),
        config_(config),
        symbol_table_(config_.model_config.tokens),
        model_(std::make_unique<OfflineMoonshineModel>(config.model_config)) {
    Init();
  }

#if __ANDROID_API__ >= 9
  OfflineRecognizerMoonshineImpl(AAssetManager *mgr,
                                 const OfflineRecognizerConfig &config)
      : OfflineRecognizerImpl(mgr, config),
        config_(config),
        symbol_table_(mgr, config_.model_config.tokens),
        model_(
            std::make_unique<OfflineMoonshineModel>(mgr, config.model_config)) {
    Init();
  }

#endif

  void Init() {
    if (config_.decoding_method == "greedy_search") {
      decoder_ =
          std::make_unique<OfflineMoonshineGreedySearchDecoder>(model_.get());
    } else {
      SHERPA_ONNX_LOGE(
          "Only greedy_search is supported at present for moonshine. Given %s",
          config_.decoding_method.c_str());
      exit(-1);
    }
  }

  std::unique_ptr<OfflineStream> CreateStream() const override {
    MoonshineTag tag;
    return std::make_unique<OfflineStream>(tag);
  }

  void DecodeStreams(OfflineStream **ss, int32_t n) const override {
    // batch decoding is not implemented yet
    for (int32_t i = 0; i != n; ++i) {
      DecodeStream(ss[i]);
    }
  }

  OfflineRecognizerConfig GetConfig() const override { return config_; }

 private:
  void DecodeStream(OfflineStream *s) const {
    auto memory_info =
        Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeDefault);

    std::vector<float> audio = s->GetFrames();

    try {
      std::array<int64_t, 2> shape{1, static_cast<int64_t>(audio.size())};

      Ort::Value audio_tensor = Ort::Value::CreateTensor(
          memory_info, audio.data(), audio.size(), shape.data(), shape.size());

      Ort::Value features =
          model_->ForwardPreprocessor(std::move(audio_tensor));

      int32_t features_len = features.GetTensorTypeAndShapeInfo().GetShape()[1];

      int64_t features_shape = 1;

      Ort::Value features_len_tensor = Ort::Value::CreateTensor(
          memory_info, &features_len, 1, &features_shape, 1);

      Ort::Value encoder_out = model_->ForwardEncoder(
          std::move(features), std::move(features_len_tensor));

      auto results = decoder_->Decode(std::move(encoder_out));

      auto r = Convert(results[0], symbol_table_);
      r.text = ApplyInverseTextNormalization(std::move(r.text));
      s->SetResult(r);
    } catch (const Ort::Exception &ex) {
      SHERPA_ONNX_LOGE(
          "\n\nCaught exception:\n\n%s\n\nReturn an empty result. Number of "
          "audio samples: %d",
          ex.what(), static_cast<int32_t>(audio.size()));
      return;
    }
  }

 private:
  OfflineRecognizerConfig config_;
  SymbolTable symbol_table_;
  std::unique_ptr<OfflineMoonshineModel> model_;
  std::unique_ptr<OfflineMoonshineDecoder> decoder_;
};

}  // namespace sherpa_onnx

#endif  // SHERPA_ONNX_CSRC_OFFLINE_RECOGNIZER_MOONSHINE_IMPL_H_