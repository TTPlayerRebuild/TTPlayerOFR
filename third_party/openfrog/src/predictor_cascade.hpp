#ifndef OPENFROG_PREDICTOR_CASCADE_HPP
#define OPENFROG_PREDICTOR_CASCADE_HPP

#include "codec_types.hpp"
#include "entropy.hpp"

#include <array>
#include <cstdint>
#include <memory>

namespace openfrog::predictor_detail {

struct CascadeStageParameters {
    std::uint32_t taps = 4;
    std::uint32_t cross_taps = 0;
    double mu = 0;
};

struct CascadeParameters {
    unsigned stages = 1;
    double preemphasis = 1;
    std::uint32_t decay = 2;
    std::uint32_t period = 1;
    std::uint32_t initial_counter = 64;
    std::array<CascadeStageParameters, 8> filters{};
};

CascadeParameters read_cascade_parameters(RangeDecoder &range, unsigned channels);

// Inputs are nearest first: input[0] is native param_2[-1]. Only the first
// dimensions entries are read. update() receives the same inputs as predict(),
// plus the actual target; no rounding or integer reconstruction is done here.
// Unsupported dimensions or floating environments throw UnsupportedCodec,
// so callers must not conceal those failures as damaged audio.
class SmallMixer {
public:
    SmallMixer(unsigned dimensions, std::uint32_t period, double alpha,
        std::uint32_t initial_counter = 0);
    ~SmallMixer();
    SmallMixer(const SmallMixer &) = delete;
    SmallMixer &operator=(const SmallMixer &) = delete;
    SmallMixer(SmallMixer &&) noexcept;
    SmallMixer &operator=(SmallMixer &&) noexcept;
    double predict(const std::array<double, 8> &inputs) const;
    void update(const std::array<double, 8> &inputs, double target);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Returns the raw binary64 prediction, before the wrapper's integer conversion,
// clipping and signed-width reconstruction. Train on the reconstructed sample.
// Calls are predict(0),update(0), then predict(1),update(1) for stereo.
// The filter regularizer is max(1, abs(all reduced endpoints in spec)).
// IEEE binary32/binary64 with nearest rounding and gradual underflow is required;
// compile with strict floating arithmetic and without multiply-add contraction.
// Unsupported capacities/environments and call-order violations are fatal
// UnsupportedCodec errors, not concealable damaged-stream errors.
class CascadePredictor {
public:
    CascadePredictor(const CascadeParameters &parameters, const PredictorSpec &spec);
    ~CascadePredictor();
    CascadePredictor(const CascadePredictor &) = delete;
    CascadePredictor &operator=(const CascadePredictor &) = delete;
    CascadePredictor(CascadePredictor &&) noexcept;
    CascadePredictor &operator=(CascadePredictor &&) noexcept;
    double predict(unsigned channel);
    void update(unsigned channel, double sample);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openfrog::predictor_detail

#endif
