/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PowerSpectrum.h"

#include <algorithm>
#include <unordered_map>

#include <glog/logging.h>

#include "SpeechUtils.h"

namespace speech {

template <typename T>
PowerSpectrum<T>::PowerSpectrum(const FeatureParams& params)
    : featParams_(params),
      dither_(params.ditherVal),
      preEmphasis_(params.preemCoef, params.numFrameSizeSamples()),
      windowing_(params.numFrameSizeSamples(), params.windowType) {
  validatePowSpecParams();
  auto nFFt = featParams_.nFft();
  inFftBuf_.resize(nFFt, 0.0);
  outFftBuf_.resize(2 * nFFt);
  fftPlan_ = fftw_plan_dft_r2c_1d(
      nFFt, inFftBuf_.data(), (fftw_complex*)outFftBuf_.data(), FFTW_MEASURE);
}

template <typename T>
std::vector<T> PowerSpectrum<T>::apply(const std::vector<T>& input) {
  auto frames = frameSignal(input, featParams_);
  if (frames.empty()) {
    return {};
  }
  return powSpectrumImpl(frames);
}

template <typename T>
std::vector<T> PowerSpectrum<T>::powSpectrumImpl(std::vector<T>& frames) {
  int64_t nSamples = featParams_.numFrameSizeSamples();
  int64_t nFrames = frames.size() / nSamples;
  int64_t nFft = featParams_.nFft();
  int64_t K = featParams_.filterFreqResponseLen();

  if (featParams_.ditherVal != 0.0) {
    frames = dither_.apply(frames);
  }
  if (featParams_.zeroMeanFrame) {
    for (size_t f = 0; f < nFrames; ++f) {
      auto begin = frames.data() + f * nSamples;
      T mean = std::accumulate(begin, begin + nSamples, 0.0);
      mean /= nSamples;
      std::transform(
          begin, begin + nSamples, begin, [mean](T x) { return x - mean; });
    }
  }
  preEmphasis_.applyInPlace(frames);
  windowing_.applyInPlace(frames);
  std::vector<T> dft(K * nFrames);
  for (size_t f = 0; f < nFrames; ++f) {
    auto begin = frames.data() + f * nSamples;
    {
      std::lock_guard<std::mutex> lock(fftMutex_);
      std::copy(begin, begin + nSamples, inFftBuf_.data());
      std::fill(outFftBuf_.begin(), outFftBuf_.end(), 0.0);
      fftw_execute(fftPlan_);

      // Copy stuff to the redundant part
      for (size_t i = K; i < nFft; ++i) {
        outFftBuf_[2 * i] = outFftBuf_[2 * nFft - 2 * i];
        outFftBuf_[2 * i + 1] = -outFftBuf_[2 * nFft - 2 * i + 1];
      }

      for (size_t i = 0; i < K; ++i) {
        dft[f * K + i] = std::sqrt(
            outFftBuf_[2 * i] * outFftBuf_[2 * i] +
            outFftBuf_[2 * i + 1] * outFftBuf_[2 * i + 1]);
      }
    }
  }
  return dft;
}

template <typename T>
std::vector<T> PowerSpectrum<T>::batchApply(
    const std::vector<T>& input,
    int64_t batchSz) {
  LOG_IF(FATAL, batchSz <= 0 || input.size() % batchSz != 0);
  int64_t N = input.size() / batchSz;
  int64_t outputSz = outputSize(N);
  std::vector<T> feat(outputSz * batchSz);

#pragma omp parallel for num_threads(batchSz)
  for (int64_t b = 0; b < batchSz; ++b) {
    auto start = input.begin() + b * N;
    std::vector<T> inputBuf(start, start + N);
    auto curFeat = apply(inputBuf);
    LOG_IF(FATAL, outputSz != curFeat.size());
    std::copy(
        curFeat.begin(), curFeat.end(), feat.begin() + b * curFeat.size());
  }
  return feat;
}

template <typename T>
FeatureParams PowerSpectrum<T>::getFeatureParams() const {
  return featParams_;
}

template <typename T>
int64_t PowerSpectrum<T>::outputSize(int64_t inputSz) {
  return featParams_.powSpecFeatSz() * featParams_.numFrames(inputSz);
}

template <typename T>
void PowerSpectrum<T>::validatePowSpecParams() const {
  LOG_IF(FATAL, featParams_.samplingFreq <= 0)
      << "'samplingfreq' has to be positive.";
  LOG_IF(FATAL, featParams_.frameSizeMs <= 0)
      << "'framesizems' has to be positive.";
  LOG_IF(FATAL, featParams_.frameStrideMs <= 0)
      << "'framestridems' has to be positive.";
  LOG_IF(FATAL, featParams_.numFrameSizeSamples() <= 0)
      << "'framesizems' too low.";
  LOG_IF(FATAL, featParams_.numFrameStrideSamples() <= 0)
      << "'framestridems' too low.";
}

template <typename T>
PowerSpectrum<T>::~PowerSpectrum() {
  fftw_destroy_plan(fftPlan_);
}

template class PowerSpectrum<float>;
template class PowerSpectrum<double>;
} // namespace speech
