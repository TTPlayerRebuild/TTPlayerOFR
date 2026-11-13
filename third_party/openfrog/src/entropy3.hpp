#ifndef OPENFROG_ENTROPY3_HPP
#define OPENFROG_ENTROPY3_HPP

#include "codec_types.hpp"
#include "entropy.hpp"

#include <memory>

namespace openfrog {

// Integer, multi-time-scale residual model (selector 3). Consumes its header
// at the range stream's CURRENT position, after the mapping/predictor headers.
// This is not a predictor or a complete audio decoder. Sources and verification:
// analysis/entropy3-implementation.md, 10004a90/10004ac0/10004ef0/10005220.
//
// width is 1..32. The original state has capacity for 1..8 channels; the audio
// container/mapping layer may impose a smaller channel limit. total is a count
// of interleaved scalar samples and must contain whole frames. RangeDecoder
// must outlive this object and must not be moved while bound.
class EntropySelector3 {
public:
    EntropySelector3(RangeDecoder &range, unsigned bit_width, unsigned channels,
        std::uint32_t total_scalar_samples);
    ~EntropySelector3();

    EntropySelector3(const EntropySelector3 &) = delete;
    EntropySelector3 &operator=(const EntropySelector3 &) = delete;

    // Like 10005220, first clamps the request to the remaining block samples,
    // THEN requires whole frames. Returns fewer samples at block end and an
    // empty vector thereafter. The clamped batch is limited by
    // kMaxEntropyBatchSamples (an implementation resource limit, not a format
    // limit; exceeding it throws UnsupportedCodec). Keeps state across calls.
    // Unsupported/unsafe parameter ranges also throw UnsupportedCodec, not a
    // recoverable damaged-block error. Does not call
    // range.finish(): the block owner remains responsible for stream framing.
    std::vector<std::int32_t> decode(std::size_t scalar_count);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openfrog

#endif
