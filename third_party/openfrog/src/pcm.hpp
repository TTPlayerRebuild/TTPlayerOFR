#ifndef OPENFROG_PCM_HPP
#define OPENFROG_PCM_HPP

#include <cstddef>
#include <cstdint>

namespace openfrog {

// Packs already-reconstructed, signed scalar samples, not sample points.
// Types 0/1, 2/3, 4/5, 6/7 denote unsigned/signed 8/16/24/32-bit PCM.
// Unsigned types add their midpoint bias with explicitly defined uint32 wrap.
// Values are not clipped; the corresponding low bytes are written little-endian.
// max16bit drops the low 8/16 bits of 24/32-bit PCM without rounding and has no
// effect on 8/16-bit PCM. Destination byte alignment does not matter.
//
// Types 8..10 copy already-reconstructed IEEE binary32 bit patterns as four
// little-endian bytes, without host floating-point arithmetic. max16bit for
// FLOAT32 is explicitly unsupported (not integer truncation or normalization).
// This is only a packing layer, NOT an audio decoder. Unknown types and float
// max16bit are rejected, including for a zero sample count.
// packed_pcm_size throws overflow_error if the output byte count cannot fit.
std::size_t packed_pcm_size(std::size_t scalar_samples, std::uint8_t sample_type,
    bool max16bit);

// Throws invalid_argument for unsupported types, null nonempty buffers, or
// insufficient capacity; throws overflow_error for unrepresentable input/output
// byte counts. All checks precede any output write. Zero samples permit null
// pointers. The caller supplies valid readable samples and writable destination
// storage for the checked counts; overlapping buffers are not guaranteed.
void pack_pcm(const std::int32_t *samples, std::size_t scalar_samples,
    std::uint8_t sample_type, bool max16bit, std::uint8_t *destination,
    std::size_t capacity);

} // namespace openfrog

#endif
