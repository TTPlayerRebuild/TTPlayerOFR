#ifndef OPENFROG_PREDICTOR_HPP
#define OPENFROG_PREDICTOR_HPP

#include "codec_types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>

namespace openfrog {

class RangeDecoder;

class PredictorError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Consumes the predictor header at the current position, after sample mapping
// and before entropy initialization. Counts are INTERLEAVED scalar samples;
// stereo calls must consist of complete frames. Predictor history survives
// successive reconstruct() calls within the same COMP block.
//
// The optional correction decoder must already be initialized at its own tag
// (0x10 or 0x11). Only selector 4 consumes correction data. Neither range decoder
// is finished here: the block owner finishes both after the final scalar sample.
// Compile these sources with strict IEEE arithmetic, no fast-math or FMA, and
// use round-to-nearest-even with gradual underflow (no FTZ/DAZ).
class Predictor {
public:
    Predictor(unsigned selector, const PredictorSpec &spec,
        std::uint32_t total_scalar_samples, RangeDecoder &main_range,
        RangeDecoder *correction = nullptr);
    ~Predictor();
    Predictor(const Predictor &) = delete;
    Predictor &operator=(const Predictor &) = delete;

    void reconstruct(std::int32_t *interleaved_residuals,
        std::size_t scalar_count);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openfrog
#endif
