#ifndef OPENFROG_PREDICTOR_LPC_HPP
#define OPENFROG_PREDICTOR_LPC_HPP

#include <cstdint>
#include <memory>

namespace openfrog {

class RangeDecoder;

namespace predictor_detail {

struct LpcParameters {
    std::uint32_t decay = 0;
    std::uint32_t period = 0;
    unsigned order = 0;       // total number of coefficients, NOT per channel
    unsigned cross_order = 0; // stereo: order - self-history length
};

// Consumes only the correlation-predictor header at the current range position.
LpcParameters read_lpc_parameters(RangeDecoder &range, unsigned channels);

// Independent binary64 correlation-matrix predictor (selectors 1, 3 and 4).
// Prediction is deliberately neither converted to an integer nor clamped.
// update() takes the actual reconstructed, reduced-domain training sample.
// Stereo callers must predict/update channel 0, then channel 1, each frame.
// Strict IEEE binary64, nearest rounding and gradual underflow are required.
// The constructor rejects dimensions that overrun the shipped fixed 128-wide
// kernel (including the genuine 192/256 table entries); it never truncates an
// order or substitutes a mathematically different larger-system solver.
// Unsafe dimensions, unsupported FP modes and allocation failure throw
// openfrog::UnsupportedCodec, so callers must not conceal them as silent audio.
class LpcPredictor {
public:
    LpcPredictor(const LpcParameters &parameters, unsigned channels);
    ~LpcPredictor();
    LpcPredictor(const LpcPredictor &) = delete;
    LpcPredictor &operator=(const LpcPredictor &) = delete;
    LpcPredictor(LpcPredictor &&) noexcept;
    LpcPredictor &operator=(LpcPredictor &&) noexcept;

    double predict(unsigned channel);
    void update(unsigned channel, double sample);

    // Persistent selector-3 flags. Suppression skips only the coefficient solve,
    // not history/correlation updates, and does not advance the solve timestamp.
    void suppress_recalculation(unsigned channel, bool suppress);
    void suppress_recalculation(bool suppress);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace predictor_detail
} // namespace openfrog

#endif
