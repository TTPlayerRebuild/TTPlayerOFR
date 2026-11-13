#ifndef OPENFROG_FLOAT_DETAILS_HPP
#define OPENFROG_FLOAT_DETAILS_HPP

#include "codec_types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

namespace openfrog {

class FloatDetailError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Implementation safety budgets, NOT format/block limits. Larger blocks can
// be reconstructed in batches without resetting the range or adaptive models.
constexpr std::size_t kMaxFloatDetailBatchSamples = 1024U * 1024U;
constexpr std::size_t kMaxFloatDetailCodedBytes = 64U * 1024U * 1024U;

// OFF v5.100 DETA reconstruction, independent of COMP/container/PCM packing.
// Source: analysis/off-b934407e/detail-implementation.md (427330 / 426aa0).
// coded starts AFTER the DETA payload's four-byte CRC and INCLUDES range tag
// 0x40. The caller validates CRC and supplies this COMP/DETA pair's scalar count.
// Owns the coded bytes and range; consumes the four header parameters once.
class FloatDetails {
public:
    FloatDetails(std::vector<std::uint8_t> coded,
        std::uint32_t total_scalar_samples);
    ~FloatDetails();

    FloatDetails(const FloatDetails &) = delete;
    FloatDetails &operator=(const FloatDetails &) = delete;

    // In-place signed integer projections -> raw IEEE binary32 bit patterns.
    // int32_t is ONLY storage for the output bits: no host float operations.
    // Counts need not be channel-aligned. An over-request is an error (not a
    // clamp); zero-length calls are no-ops, including after successful finish.
    // The last sample finishes the range, without requiring EOF or checking CRC.
    // A stream error poisons the object and may leave a partially written batch;
    // callers must discard that block. No implicit silence/error concealment.
    // Safety budgets throw UnsupportedCodec; bad_alloc is not translated.
    void reconstruct(std::int32_t *projected, std::size_t scalar_count);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openfrog

#endif
