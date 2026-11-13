#ifndef OPENFROG_CODEC_TYPES_HPP
#define OPENFROG_CODEC_TYPES_HPP

#include <array>
#include <cstdint>
#include <stdexcept>

namespace openfrog {

// Unsupported or unsafe algorithm parameters are fatal, never concealed as
// successfully repaired/silent audio. Ordinary damaged-bitstream errors can
// still follow the native decoder's explicit error-concealment path.
class UnsupportedCodec : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Bounds after sample mapping, in the domain reconstructed by the predictor.
// Sample counts in the internal pipeline are interleaved scalar samples.
struct PredictorSpec {
    unsigned channels = 0;
    unsigned bit_width = 0;
    std::array<std::int32_t, 2> minimum{{0, 0}};
    std::array<std::int32_t, 2> maximum{{0, 0}};
};

} // namespace openfrog
#endif
