#include "pcm.hpp"

#include <limits>
#include <stdexcept>

namespace openfrog {
namespace {

// Independent expression of the integer packing switch in retained REA
// analysis/pseudocode/functions/0x100019c0.c, cases 0..7. No decoder is invoked.
struct PcmLayout {
    std::uint32_t output_bytes;
    std::uint32_t bias;
    std::uint32_t shift;
};

PcmLayout layout_for(std::uint8_t sample_type, bool max16bit) {
    if (sample_type > 7) {
        if (sample_type <= 10) {
            throw std::invalid_argument("floating-point PCM packing is not implemented");
        }
        throw std::invalid_argument("unknown PCM sample type");
    }
    const std::uint32_t source_bytes = static_cast<std::uint32_t>(sample_type) / 2U + 1U;
    const bool truncate = max16bit && source_bytes > 2U;
    const std::uint32_t bias = (sample_type & 1U) == 0U
        ? (std::uint32_t{1} << (source_bytes * 8U - 1U)) : std::uint32_t{0};
    return PcmLayout{truncate ? 2U : source_bytes, bias,
        truncate ? (source_bytes - 2U) * 8U : 0U};
}

std::size_t checked_bytes(std::size_t count, std::size_t bytes_per_sample) {
    if (count > (std::numeric_limits<std::size_t>::max)() / bytes_per_sample) {
        throw std::overflow_error("PCM byte count exceeds size_t");
    }
    return count * bytes_per_sample;
}

} // namespace

std::size_t packed_pcm_size(std::size_t scalar_samples, std::uint8_t sample_type,
    bool max16bit) {
    const PcmLayout layout = layout_for(sample_type, max16bit);
    return checked_bytes(scalar_samples, layout.output_bytes);
}

void pack_pcm(const std::int32_t *samples, std::size_t scalar_samples,
    std::uint8_t sample_type, bool max16bit, std::uint8_t *destination,
    std::size_t capacity) {
    const PcmLayout layout = layout_for(sample_type, max16bit);
    const std::size_t required = checked_bytes(scalar_samples, layout.output_bytes);
    // Even an 8-bit destination must not permit an overflowing int32 input size.
    (void)checked_bytes(scalar_samples, sizeof(std::int32_t));
    if (!scalar_samples) {
        return;
    }
    if (!samples || !destination) {
        throw std::invalid_argument("nonempty PCM input/output buffer is null");
    }
    if (capacity < required) {
        throw std::invalid_argument("PCM output capacity is insufficient");
    }

    for (std::size_t index = 0; index < scalar_samples; ++index) {
        // Signed-to-unsigned conversion and unsigned addition are modulo 2^32.
        // Neither negative signed shifts nor signed overflowing additions occur.
        std::uint32_t value = static_cast<std::uint32_t>(samples[index]);
        value += layout.bias;
        value >>= layout.shift;
        for (std::uint32_t byte = 0; byte < layout.output_bytes; ++byte) {
            *destination++ = static_cast<std::uint8_t>(value);
            value >>= 8U;
        }
    }
}

} // namespace openfrog
