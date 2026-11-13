#ifndef OPENFROG_MAPPING_HPP
#define OPENFROG_MAPPING_HPP

#include "codec_types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>

namespace openfrog {

class RangeDecoder;

class MappingError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Allocation/work bound, not a claimed OFR format limit. Only enabled nonlinear
// alphabets need enumeration; an affine or disabled nonlinear range may be 32 bit.
constexpr std::size_t kMaxMappingAlphabet = 16U * 1024U * 1024U;

// Consumes the complete mapping header at range's CURRENT position. The caller
// must subsequently read predictor and entropy headers, in that order. spec()
// describes the reduced/predictor domain, NOT the final PCM domain. apply() is
// the last reconstruction stage, after entropy decoding and prediction.
// Selector 1 is affine; selector 2 is a nonlinear available-integer alphabet,
// not an IEEE floating-PCM conversion. Stereo buffers contain complete frames.
// Unsupported/unsafe configurations and resource guards throw UnsupportedCodec
// (fatal, not concealable); malformed headers/ranks throw MappingError or the
// RangeDecoder's EntropyError. No error path manufactures replacement samples.
class SampleMapping {
public:
    SampleMapping(unsigned selector, unsigned container_bits, unsigned channels,
        bool force_full_range, RangeDecoder &range);
    ~SampleMapping();
    SampleMapping(const SampleMapping &) = delete;
    SampleMapping &operator=(const SampleMapping &) = delete;
    SampleMapping(SampleMapping &&) noexcept;
    SampleMapping &operator=(SampleMapping &&) noexcept;

    const PredictorSpec &spec() const;
    void apply(std::int32_t *interleaved_samples, std::size_t scalar_count);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openfrog

#endif
