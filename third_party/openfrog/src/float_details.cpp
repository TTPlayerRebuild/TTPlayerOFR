#include "float_details.hpp"
#include "entropy.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace openfrog {
namespace {

static_assert(sizeof(std::int32_t) == sizeof(std::uint32_t),
    "Float details require 32-bit integer storage");

[[noreturn]] void damaged(const char *message) {
    throw FloatDetailError(message);
}

std::vector<std::uint8_t> checked_input(std::vector<std::uint8_t> input) {
    if (input.size() > kMaxFloatDetailCodedBytes) {
        throw UnsupportedCodec("DETA coded input exceeds implementation budget");
    }
    return input;
}

std::uint32_t magnitude(std::uint32_t signed_bits) noexcept {
    // Unsigned negation is well-defined even for INT_MIN. Callers validate the
    // resulting magnitude before any shifts or signed conversions.
    return signed_bits & 0x80000000U ? 0U - signed_bits : signed_bits;
}

unsigned log2_nonzero(std::uint32_t value) noexcept {
    unsigned result = 0;
    while (value > 1) {
        value >>= 1;
        ++result;
    }
    return result;
}

// 426aa0, expressed without its redundant read/modify/write of the output sign
// bit. Every successful branch returns ALL 32 bits, never an uninitialized word.
std::uint32_t compose(int exponent, std::uint32_t mantissa) {
    if (exponent == -150) {
        if (mantissa > 1) { damaged("invalid DETA signed zero"); }
        return mantissa << 31;
    }
    if (exponent == 128) {
        if (mantissa > 0x00ffffffU) { damaged("invalid DETA nonfinite payload"); }
        return 0x7f800000U | (mantissa >> 1) | (mantissa << 31);
    }
    if (exponent < -149 || exponent > 127) {
        damaged("invalid DETA finite exponent");
    }
    const auto sign = mantissa & 0x80000000U;
    auto value = magnitude(mantissa);
    if (value < 0x00800000U || value > 0x00ffffffU) {
        damaged("invalid DETA normalized mantissa");
    }
    // Native uses SAR on an EVEN signed mantissa. Exact unsigned division of
    // its magnitude preserves the sign and avoids implementation-defined SAR.
    while (exponent < -126) {
        if (value & 1U) { damaged("inexact DETA subnormal reconstruction"); }
        value >>= 1;
        ++exponent;
    }
    if (value < 0x00800000U) { return sign | value; }
    return sign | (static_cast<std::uint32_t>(exponent + 127) << 23)
        | (value & 0x007fffffU);
}

unsigned detail_bits(int exponent, int minimum, unsigned trailing) {
    const int count = (std::min)(exponent - minimum,
        23 - static_cast<int>(trailing));
    if (count < 0 || count > 23) {
        damaged("invalid DETA detail bit count");
    }
    return static_cast<unsigned>(count);
}

} // namespace

struct FloatDetails::Impl {
    RangeDecoder range;
    AdaptiveTree zero_sign{2, 0x8000};
    AdaptiveTree exponent_model{279, 0x8000};
    std::uint32_t remaining;
    unsigned projection_bits = 0;
    unsigned trailing_zeros = 0;
    int minimum_exponent = 0;
    int maximum_exponent = 0;
    bool failed = false;

    Impl(std::vector<std::uint8_t> input, std::uint32_t count)
        : range(checked_input(std::move(input))), remaining(count) {
        if (range.state().tag != 0x40) { damaged("invalid DETA range tag"); }
        projection_bits = range.byteSymbol();
        trailing_zeros = range.byteSymbol();
        minimum_exponent = static_cast<int>(range.uniform(512)) - 150;
        maximum_exponent = static_cast<int>(range.uniform(512)) - 150;
        // 426c90 produces finite quantum exponents -149..127; no-finite blocks
        // use (0,0). Do not emulate x86 masked/negative shifts for corrupt data.
        if (projection_bits > 23 || trailing_zeros > 23
            || projection_bits > 23 - trailing_zeros
            || minimum_exponent < -149 || maximum_exponent > 127
            || minimum_exponent > maximum_exponent
            || static_cast<int>(projection_bits) > maximum_exponent - minimum_exponent) {
            damaged("invalid DETA block parameters");
        }
        if (!remaining) { range.finish(); }
    }

    std::uint32_t one(std::int32_t projected) {
        if (!projected) {
            const int exponent = static_cast<int>(exponent_model.decode(range)) - 150;
            if (exponent == -150) { return compose(exponent, zero_sign.decode(range)); }
            if (exponent == 128) { return compose(exponent, range.bits(24)); }
            const auto sign = range.uniform(2);
            const auto count = detail_bits(exponent, minimum_exponent, trailing_zeros);
            auto mantissa = 0x00800000U + (range.bits(count) << (23 - count));
            if (sign) { mantissa = 0U - mantissa; }
            return compose(exponent, mantissa);
        }

        if (projected == (std::numeric_limits<std::int32_t>::min)()) {
            damaged("INT_MIN is not a valid OFF integer projection");
        }
        const auto input = static_cast<std::uint32_t>(projected);
        const auto absolute = magnitude(input);
        // The encoder projects at most the leading 24 normalized mantissa bits.
        if (absolute > 0x00ffffffU) { damaged("DETA projection exceeds 24 bits"); }
        const auto leading = log2_nonzero(absolute);
        const int exponent = static_cast<int>(leading)
            - static_cast<int>(projection_bits) + maximum_exponent;
        if (exponent < -149 || exponent > 127) {
            damaged("DETA projected exponent is not finite");
        }
        const auto count = detail_bits(exponent, minimum_exponent, trailing_zeros);
        if (count < leading) { damaged("DETA projection/detail width mismatch"); }
        const auto extra = count - leading;
        // extra may be zero: native bits(0) consumes nothing, unlike uniform(1).
        const auto low = range.bits(extra);
        const auto wide = ((static_cast<std::uint64_t>(absolute) << extra) | low)
            << (23 - count);
        if (wide > 0x00ffffffU) { damaged("DETA reconstructed mantissa overflow"); }
        auto mantissa = static_cast<std::uint32_t>(wide);
        if (projected < 0) { mantissa = 0U - mantissa; }
        return compose(exponent, mantissa);
    }
};

FloatDetails::FloatDetails(std::vector<std::uint8_t> coded,
    std::uint32_t total_scalar_samples) {
    try {
        impl_ = std::make_unique<Impl>(std::move(coded), total_scalar_samples);
    } catch (const EntropyError &error) {
        throw FloatDetailError(error.what());
    }
}

FloatDetails::~FloatDetails() = default;

void FloatDetails::reconstruct(std::int32_t *projected, std::size_t scalar_count) {
    auto &state = *impl_;
    if (state.failed) { damaged("DETA decoder is failed"); }
    if (scalar_count > kMaxFloatDetailBatchSamples) {
        throw UnsupportedCodec("DETA batch exceeds implementation budget");
    }
    if (scalar_count > state.remaining || (scalar_count && !projected)) {
        damaged("invalid DETA reconstruction request");
    }
    if (!scalar_count) { return; }
    try {
        for (std::size_t i = 0; i < scalar_count; ++i) {
            const auto bits = state.one(projected[i]);
            std::memcpy(projected + i, &bits, sizeof(bits));
            --state.remaining;
        }
        if (!state.remaining) { state.range.finish(); }
    } catch (const EntropyError &error) {
        state.failed = true;
        throw FloatDetailError(error.what());
    } catch (const FloatDetailError &) {
        state.failed = true;
        throw;
    }
}

} // namespace openfrog
