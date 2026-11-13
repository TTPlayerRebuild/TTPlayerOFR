#include "mapping.hpp"
#include "entropy.hpp"

#include <algorithm>
#include <array>
#include <cfenv>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <utility>
#include <vector>

#if defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2) || defined(__SSE2__)
#include <xmmintrin.h>
#define OPENFROG_MAPPING_SSE_ENVIRONMENT 1
#endif

#if defined(__FAST_MATH__) || (defined(_M_FP_FAST) && _M_FP_FAST)
#error OptimFROG nonlinear mapping requires strict IEEE floating-point arithmetic.
#endif

namespace openfrog {
namespace {

static_assert(sizeof(double) == sizeof(std::uint64_t)
    && std::numeric_limits<double>::is_iec559
    && std::numeric_limits<double>::digits == 53,
    "Nonlinear mapping requires IEEE-754 binary64 double");

std::int32_t signed32(std::uint32_t value) noexcept {
    if (value <= UINT32_C(0x7fffffff)) {
        return static_cast<std::int32_t>(value);
    }
    return -1 - static_cast<std::int32_t>(~value);
}

std::int32_t read_signed(RangeDecoder &range, unsigned bits) {
    auto value = range.bits(bits);
    if (bits < 32 && (value & (UINT32_C(1) << (bits - 1)))) {
        value |= UINT32_MAX << bits;
    }
    return signed32(value);
}

std::int32_t checked32(std::int64_t value) {
    if (value < INT32_MIN || value > INT32_MAX) {
        throw UnsupportedCodec("mapping reduced endpoint exceeds safe int32 domain");
    }
    return static_cast<std::int32_t>(value);
}

std::int32_t floor_div(std::int64_t value, std::int64_t divisor) {
    // Even in the __alldvrm path, 10010e1a/10010e41 and the stereo
    // equivalents TEST ONLY ECX, the SIGNED LOW WORD of the remainder.
    // This differs from mathematical floor for scales above INT32_MAX.
    const auto remainder = signed32(static_cast<std::uint32_t>(value % divisor));
    return checked32(value / divisor - (remainder < 0 ? 1 : 0));
}

std::int32_t ceil_div(std::int64_t value, std::int64_t divisor) {
    const auto remainder = signed32(static_cast<std::uint32_t>(value % divisor));
    return checked32(value / divisor + (remainder > 0 ? 1 : 0));
}

unsigned signed_width(std::int32_t minimum, std::int32_t maximum) noexcept {
    auto magnitude = [](std::int32_t value) {
        const auto word = static_cast<std::uint32_t>(value);
        return value < 0 ? ~word : word;
    };
    auto value = (std::max)(magnitude(minimum), magnitude(maximum));
    unsigned bits = 1;
    while (value) {
        ++bits;
        value >>= 1;
    }
    return bits;
}

void require_nearest() {
    if (std::fegetround() != FE_TONEAREST) {
        throw UnsupportedCodec("nonlinear mapping requires round-to-nearest arithmetic");
    }
#if defined(OPENFROG_MAPPING_SSE_ENVIRONMENT)
    // x87 and SSE controls can be changed independently on x86. fegetround()
    // alone need not reveal a mismatched MXCSR rounding mode.
    if ((_mm_getcsr() & 0xe040U) != 0) {
        throw UnsupportedCodec("nonlinear mapping requires nearest SSE rounding, no FTZ/DAZ");
    }
#endif
    volatile double tiny = std::numeric_limits<double>::denorm_min();
    volatile double doubled = tiny + tiny;
    const double stored = doubled;
    std::uint64_t representation = 0;
    std::memcpy(&representation, &stored, sizeof(representation));
    if (representation != 2) {
        throw UnsupportedCodec("nonlinear mapping requires gradual underflow (no FTZ/DAZ)");
    }
}

// CVT(T)SD2SI's masked invalid result, without an out-of-range C++ cast.
std::int32_t sse_integer(double value) noexcept {
    if (!(value >= -2147483648.0 && value < 2147483648.0)) {
        return INT32_MIN;
    }
    return static_cast<std::int32_t>(value);
}

double nonlinear_parameter(std::uint32_t low, std::uint32_t high) {
    const auto word = (static_cast<std::uint64_t>(high) << 32) | low;
    // 1001228b is FILD qword, i.e. SIGNED, even though the two input words
    // are unsigned byte-symbol assemblies. The qword is rounded to double
    // before division by 2^52 (10012299 / 100122a2).
    const std::int64_t integer = (word >> 63)
        ? -1 - static_cast<std::int64_t>(~word)
        : static_cast<std::int64_t>(word);
    volatile double rounded = static_cast<double>(integer);
    volatile double parameter = rounded / 4503599627370496.0;
    return parameter;
}

std::int32_t transform(double parameter, std::int32_t value, unsigned selector) {
    volatile double product = parameter * static_cast<double>(value);
    const double x = product;
    switch (selector) {
    case 0: // 10011570; 6 is the very same function pointer.
    case 6:
        return sse_integer(std::floor(x));
    case 1: // 100115e0 -> ceil -> _ftol2_sse
        return sse_integer(std::ceil(x));
    case 2: // Branch is on the input INTEGER's sign, not the product's sign.
        return sse_integer(value < 0 ? std::ceil(x) : std::floor(x));
    case 3:
        return sse_integer(value < 0 ? std::floor(x) : std::ceil(x));
    case 4: { // 10011730: separate rounded ADD, then floor, not std::round.
        volatile double shifted = x + 0.5;
        return sse_integer(std::floor(shifted));
    }
    case 5: // 100117b0: CVTSD2SI, ties to even in the required environment.
        return sse_integer(std::nearbyint(x));
    default:
        throw UnsupportedCodec("invalid/unsafe nonlinear mapping transform");
    }
}

struct Channel {
    std::int32_t minimum = 0, maximum = 0;
    std::uint32_t scale = 1;
    std::int32_t offset = 0;
    bool enabled = false;
    // Negative ranks are -1,-2,...; positive ranks are 1,2,...; rank 0 is
    // always 0, even when zero was not selected by the corrected alphabet.
    std::vector<std::int32_t> negative, positive;
};

void read_alphabet(Channel &channel, RangeDecoder &range, unsigned selector,
    double parameter) {
    const auto count64 = static_cast<std::int64_t>(channel.maximum)
        - channel.minimum + 1;
    if (count64 > static_cast<std::int64_t>(kMaxMappingAlphabet)) {
        throw UnsupportedCodec("nonlinear mapping alphabet exceeds implementation budget");
    }
    const auto count = static_cast<std::size_t>(count64);
    std::vector<std::uint8_t> candidate(count, 0), selected(count, 0);
    const auto index = [&channel](std::int64_t value) {
        return static_cast<std::size_t>(value - channel.minimum);
    };
    for (std::int64_t value = channel.minimum; value <= channel.maximum; ++value) {
        const auto mapped = transform(parameter, static_cast<std::int32_t>(value), selector);
        if (mapped >= channel.minimum && mapped <= channel.maximum) {
            candidate[index(mapped)] = 1;
        }
    }

    // 10011cc0: four independent models, RESET for each enabled channel.
    AdaptiveTree membership(2, 0x8000), replace_next(2, 0x8000);
    AdaptiveTree replace_previous(2, 0x8000), replace_either(3, 0x8000);
    for (std::int64_t value = channel.minimum; value <= channel.maximum; ++value) {
        const auto i = index(value);
        const bool previous = i != 0 && selected[i - 1] == 1;
        const bool next = i + 1 < count && candidate[i + 1] == 1;
        if (candidate[i]) {
            selected[i] = 1;
        } else {
            selected[i] = static_cast<std::uint8_t>(membership.decode(range));
            if (!selected[i]) {
                continue;
            }
            if (!previous && next) {
                if (replace_next.decode(range) == 0) {
                    selected[i + 1] = 0;
                    ++value; // Skip that candidate; it must not be reinserted.
                }
            } else if (previous && !next) {
                if (replace_previous.decode(range) == 0) {
                    selected[i - 1] = 0;
                }
            } else if (previous && next) {
                const auto choice = replace_either.decode(range);
                if (choice == 0) {
                    selected[i - 1] = 0;
                } else if (choice == 1) {
                    selected[i + 1] = 0;
                    ++value;
                }
            }
        }
    }
    for (std::int64_t value = (std::max)(INT64_C(1),
        static_cast<std::int64_t>(channel.minimum)); value <= channel.maximum; ++value) {
        if (selected[index(value)]) {
            channel.positive.push_back(static_cast<std::int32_t>(value));
        }
    }
    for (std::int64_t value = (std::min)(INT64_C(-1),
        static_cast<std::int64_t>(channel.maximum)); value >= channel.minimum; --value) {
        if (selected[index(value)]) {
            channel.negative.push_back(static_cast<std::int32_t>(value));
        }
    }
}

} // namespace

struct SampleMapping::Impl {
    unsigned selector;
    bool full_range;
    bool narrow = true;
    bool any_table = false;
    PredictorSpec specification;
    std::array<Channel, 2> channel;

    Impl(unsigned selected, unsigned bits, unsigned channels, bool force,
        RangeDecoder &range) : selector(selected), full_range(force) {
        if ((selected != 1 && selected != 2) || bits < 1 || bits > 32
            || channels < 1 || channels > 2) {
            throw UnsupportedCodec("mapping requires selector 1/2, width 1..32, channels 1/2");
        }
        if (selected == 2) {
            require_nearest();
        }
        specification.channels = channels;
        for (unsigned c = 0; c < channels; ++c) {
            auto &ch = channel[c];
            ch.minimum = read_signed(range, bits);
            ch.maximum = read_signed(range, bits);
            if (ch.minimum > ch.maximum) {
                throw MappingError("mapping minimum exceeds maximum");
            }
            if (selected == 1) {
                if (range.smallBits(1)) {
                    ch.scale = range.bits(bits);
                    ch.offset = read_signed(range, bits);
                }
                if (ch.scale == 0) {
                    throw MappingError("zero affine mapping scale");
                }
            } else {
                ch.enabled = range.smallBits(1) != 0;
                if (ch.enabled) {
                    // 10012070 unconditionally stores table[0], then walks
                    // from +/-1. A zero-excluding allocation is unsafe in the
                    // original too; do not silently invent alternate semantics.
                    if (ch.minimum > 0 || ch.maximum < 0) {
                        throw UnsupportedCodec("enabled nonlinear alphabet must include zero");
                    }
                    any_table = true;
                    const auto transform_selector = range.smallBits(4);
                    // The 4-bit field does NOT imply a 16-function table.
                    // PE 10023ff8 has seven entries, then constants/strings.
                    if (transform_selector > 6) {
                        throw UnsupportedCodec("invalid/unsafe nonlinear mapping transform");
                    }
                    const auto low = range.u32();
                    const auto high = range.u32();
                    read_alphabet(ch, range, transform_selector,
                        nonlinear_parameter(low, high));
                }
            }
        }

        if (selected == 1) {
            bool original_narrow = true;
            for (unsigned c = 0; c < channels; ++c) {
                original_narrow = original_narrow && channel[c].minimum >= -8388608
                    && channel[c].maximum <= 8388607;
            }
            for (unsigned c = 0; c < channels; ++c) {
                auto &ch = channel[c];
                if (original_narrow) {
                    // The native short path uses signed IDIV and 32-bit SUB.
                    // Valid <=24-bit streams cannot overflow either operation;
                    // keep the native wrap explicit for wider container headers.
                    const auto divisor = signed32(ch.scale);
                    const auto lo = signed32(static_cast<std::uint32_t>(ch.minimum)
                        - static_cast<std::uint32_t>(ch.offset));
                    const auto hi = signed32(static_cast<std::uint32_t>(ch.maximum)
                        - static_cast<std::uint32_t>(ch.offset));
                    specification.minimum[c] = floor_div(lo, divisor);
                    specification.maximum[c] = ceil_div(hi, divisor);
                } else {
                    specification.minimum[c] = floor_div(
                        static_cast<std::int64_t>(ch.minimum) - ch.offset, ch.scale);
                    specification.maximum[c] = ceil_div(
                        static_cast<std::int64_t>(ch.maximum) - ch.offset, ch.scale);
                }
                if (specification.minimum[c] > specification.maximum[c]) {
                    throw MappingError("invalid affine reduced range");
                }
                // This happens AFTER computing predictor bounds, never before.
                if (force) {
                    ch.maximum = bits == 32 ? INT32_MAX
                        : static_cast<std::int32_t>((UINT32_C(1) << (bits - 1)) - 1);
                    ch.minimum = -1 - ch.maximum;
                }
                narrow = narrow && ch.minimum >= -8388608 && ch.maximum <= 8388607;
            }
        } else {
            for (unsigned c = 0; c < channels; ++c) {
                const auto &ch = channel[c];
                specification.minimum[c] = ch.enabled
                    ? -static_cast<std::int32_t>(ch.negative.size()) : ch.minimum;
                specification.maximum[c] = ch.enabled
                    ? static_cast<std::int32_t>(ch.positive.size()) : ch.maximum;
            }
        }
        auto minimum = specification.minimum[0];
        auto maximum = specification.maximum[0];
        if (channels == 2) {
            minimum = (std::min)(minimum, specification.minimum[1]);
            maximum = (std::max)(maximum, specification.maximum[1]);
        }
        specification.bit_width = signed_width(minimum, maximum);
    }

    std::int32_t lookup(std::int32_t value, unsigned c) const {
        const auto &ch = channel[c];
        if (!ch.enabled) {
            if (value < ch.minimum || value > ch.maximum) {
                throw MappingError("nonlinear identity index outside mapping table");
            }
            if (value == 0x0fffffff) {
                throw MappingError("nonlinear mapping sentinel");
            }
            return value;
        }
        if (!value) {
            return 0;
        }
        const auto rank = value < 0 ? -static_cast<std::int64_t>(value) : value;
        const auto &table = value < 0 ? ch.negative : ch.positive;
        if (rank > static_cast<std::int64_t>(table.size())) {
            throw MappingError("nonlinear rank outside mapping table");
        }
        const auto result = table[static_cast<std::size_t>(rank - 1)];
        if (result == 0x0fffffff) {
            throw MappingError("nonlinear mapping sentinel");
        }
        return result;
    }
};

SampleMapping::SampleMapping(unsigned selector, unsigned container_bits,
    unsigned channels, bool force_full_range, RangeDecoder &range) try
    : impl_(std::make_unique<Impl>(selector, container_bits, channels,
        force_full_range, range)) {}
catch (const std::bad_alloc &) {
    throw UnsupportedCodec("insufficient memory for sample mapping");
}

SampleMapping::~SampleMapping() = default;
SampleMapping::SampleMapping(SampleMapping &&) noexcept = default;
SampleMapping &SampleMapping::operator=(SampleMapping &&) noexcept = default;

const PredictorSpec &SampleMapping::spec() const {
    if (!impl_) {
        throw MappingError("moved-from sample mapping");
    }
    return impl_->specification;
}

void SampleMapping::apply(std::int32_t *samples, std::size_t count) {
    const unsigned channels = spec().channels;
    if ((!samples && count) || count % channels) {
        throw MappingError("invalid mapping buffer or partial stereo frame");
    }
    if (impl_->selector == 2) {
        if (impl_->any_table) {
            // Validate first, so bad ranks cannot leave a partly modified buffer.
            for (std::size_t i = 0; i < count; ++i) {
                (void)impl_->lookup(samples[i], static_cast<unsigned>(i % channels));
            }
            for (std::size_t i = 0; i < count; ++i) {
                samples[i] = impl_->lookup(samples[i], static_cast<unsigned>(i % channels));
            }
        }
        return; // All disabled: the original stage is a no-op, not a clamp.
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto &ch = impl_->channel[i % channels];
        if (ch.scale == 1 && ch.offset == 0 && !impl_->full_range) {
            continue;
        }
        std::int64_t mapped;
        if (impl_->narrow) {
            mapped = signed32(static_cast<std::uint32_t>(samples[i]) * ch.scale
                + static_cast<std::uint32_t>(ch.offset));
        } else {
            // Signed int64 factor: negative samples must NOT become unsigned.
            mapped = static_cast<std::int64_t>(samples[i])
                * static_cast<std::int64_t>(ch.scale) + ch.offset;
        }
        samples[i] = static_cast<std::int32_t>((std::max)(
            static_cast<std::int64_t>(ch.minimum), (std::min)(mapped,
                static_cast<std::int64_t>(ch.maximum))));
    }
}

} // namespace openfrog
