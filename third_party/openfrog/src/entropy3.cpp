#include "entropy3.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace openfrog {
namespace {

// The native log2 sequence deliberately maps zero to context/exponent zero.
unsigned log2_or_zero(std::uint32_t value) noexcept {
    unsigned result = 0;
    while (value > 1) {
        value >>= 1;
        ++result;
    }
    return result;
}

std::uint32_t low32(std::uint64_t value) noexcept {
    return static_cast<std::uint32_t>(value & UINT64_C(0xffffffff));
}

// C++17 does not specify signed negative right shifts. All differences here
// are in (-2^48, 2^48), so negation and the rounding addition cannot overflow.
std::int64_t arithmetic_shift(std::int64_t value, unsigned count) noexcept {
    const auto divisor = INT64_C(1) << count;
    return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
}

struct Parameters {
    std::uint32_t threshold = 0;
    std::uint32_t multiplier = 0;
    std::uint32_t alphabet = 0;
};

Parameters read_parameters(RangeDecoder &range) {
    // 10004bdf..10004c87 / 10004d25..10004df7. The first bit is
    // stored natively as 1 or 2 at channel+0x45c, but 10004ef0 never reads
    // that field. It MUST be consumed, not guessed or used as a multiplier.
    range.smallBits(1);
    Parameters result;
    result.threshold = std::uint32_t(1) << (range.smallBits(3) + 12U);
    result.multiplier = range.byteSymbol() + 1U;
    result.alphabet = range.smallBits(7) + 2U;
    return result;
}

struct ResidualModel {
    Parameters parameters;
    AdaptiveTree tree;

    explicit ResidualModel(Parameters value)
        : parameters(value), tree(value.alphabet, value.threshold) {}
};

struct Channel {
    bool use_previous_scale = false;
    std::uint32_t block_length = 0;
    std::uint32_t block_remaining = 0;
    unsigned scale = 0;
    std::array<std::int64_t, 8> averages{}; // unsigned magnitudes, stored Q16
    std::array<int, 32> context_to_model{};
    std::vector<ResidualModel> models;
    std::vector<AdaptiveTree> scale_models; // 9 trees, indexed by previous scale
};

Channel read_channel(RangeDecoder &range, unsigned bit_width) {
    Channel result;
    result.use_previous_scale = range.smallBits(1) != 0;
    const auto length_selector = range.smallBits(4);
    result.block_length = std::uint32_t(1) << (6U + (length_selector >> 1));
    if (length_selector & 1U) {
        result.block_length += result.block_length >> 1;
    }
    const bool shared_parameters = range.smallBits(1) != 0;
    const auto common = shared_parameters ? read_parameters(range) : Parameters{};

    // Assembly fixes the misleading decompiler induction variables: one bit
    // per exponent, a new model only on 1; 0 reuses the preceding model. Each
    // channel restarts at model=-1. There is no implicit/default first model;
    // an unmapped prefix is harmless if only zero blocks are ever decoded.
    result.models.reserve(bit_width);
    for (unsigned exponent = 0; exponent < bit_width; ++exponent) {
        if (range.smallBits(1)) {
            result.models.emplace_back(shared_parameters ? common : read_parameters(range));
        }
        result.context_to_model[exponent] = static_cast<int>(result.models.size()) - 1;
    }

    // Hidden arguments at 10004e8c..10004ea0: contexts=9, alphabet=9,
    // threshold=0x8000. 10009ff8 initializes EACH of the nine trees.
    result.scale_models.reserve(9);
    for (unsigned i = 0; i < 9; ++i) {
        result.scale_models.emplace_back(9, 0x8000);
    }
    return result;
}

} // namespace

struct EntropySelector3::Impl {
    RangeDecoder &range;
    unsigned bit_width;
    unsigned channels;
    std::uint32_t total;
    std::uint32_t decoded = 0;
    bool failed = false;
    std::vector<Channel> state;

    Impl(RangeDecoder &input, unsigned width, unsigned channel_count,
        std::uint32_t scalar_total)
        : range(input), bit_width(width), channels(channel_count), total(scalar_total) {
        if (width < 1 || width > 32 || channels < 1 || channels > 8) {
            throw UnsupportedCodec("selector 3 supports width 1..32 and channels 1..8");
        }
        if (total % channels) {
            throw EntropyError("selector 3 sample total has a partial frame");
        }
        state.reserve(channels);
        for (unsigned i = 0; i < channels; ++i) {
            state.push_back(read_channel(range, bit_width));
        }
    }

    std::int32_t decode_one(Channel &channel) {
        if (!channel.block_remaining) {
            channel.scale = channel.scale_models.at(channel.scale).decode(range);
            channel.block_remaining = channel.block_length;
            if (channel.scale == 8) {
                channel.averages.fill(0);
            }
        }
        --channel.block_remaining;
        if (channel.scale == 8) {
            return 0; // no residual tree/tail reads or smoothing in zero blocks
        }

        auto average = channel.averages.at(channel.scale);
        if (channel.use_previous_scale && channel.scale != 0) {
            average = (std::max)(average, channel.averages.at(channel.scale - 1));
        }
        const auto integral = static_cast<std::uint32_t>(average >> 16);
        const auto exponent = log2_or_zero(integral);
        if (exponent >= bit_width) {
            throw EntropyError("selector 3 mean exceeds configured context width");
        }
        const auto model_index = channel.context_to_model[exponent];
        if (model_index < 0) {
            throw EntropyError("selector 3 selected an unmapped context");
        }
        auto &model = channel.models.at(static_cast<std::size_t>(model_index));
        // __allmul followed by SHRD 24: multiplier * Q16 / 2^24, then +1.
        const auto scaled = static_cast<std::uint64_t>(average) * model.parameters.multiplier;
        const auto step_minus_one = low32(scaled >> 24);
        const auto step = low32(static_cast<std::uint64_t>(step_minus_one) + 1U);
        if (!step) {
            throw UnsupportedCodec("unsafe selector 3 residual step overflow");
        }
        const auto symbol = model.tree.decode(range);
        std::uint32_t value;
        if (symbol == model.parameters.alphabet - 1U) {
            // Native IMUL is modulo 2^32, including when the product is zero.
            const auto base = log2_or_zero(low32(
                static_cast<std::uint64_t>(model.parameters.alphabet - 1U) * step));
            if (base >= bit_width) {
                throw EntropyError("selector 3 escape exceeds configured width");
            }
            const auto bits = base + range.uniform(bit_width - base);
            value = (std::uint32_t(1) << bits) + range.bits(bits);
        } else {
            std::uint32_t tail;
            if (step <= 0x4000U) {
                tail = range.uniform(step); // step=1 still normalizes
            } else {
                // 100050d2..100050fe: do not use uniform(step) or bits(log2).
                // Both additions are DWORD additions in the original. Reject
                // their zero-divisor/empty-alphabet cases rather than invoke UB.
                const auto chunk = low32(static_cast<std::uint64_t>(step_minus_one)
                    + 0x4000U) >> 14;
                if (!chunk) {
                    throw UnsupportedCodec("unsafe selector 3 segmented tail divisor overflow");
                }
                const auto groups = low32(static_cast<std::uint64_t>(chunk - 1U)
                    + step) / chunk;
                if (!groups) {
                    throw UnsupportedCodec("unsafe selector 3 segmented tail alphabet overflow");
                }
                const auto high = range.uniform(groups);
                const auto low = range.uniform(chunk);
                tail = low32(static_cast<std::uint64_t>(high) * chunk + low);
            }
            // No invented width clamp: 10004ef0 uses DWORD multiply/add here.
            value = low32(static_cast<std::uint64_t>(symbol) * step + tail);
        }

        const auto target = static_cast<std::int64_t>(value) * INT64_C(65536);
        for (unsigned i = 0; i < channel.averages.size(); ++i) {
            channel.averages[i] += arithmetic_shift(target - channel.averages[i], i + 3U);
        }
        return unfold(value);
    }

    std::vector<std::int32_t> decode(std::size_t requested) {
        const auto rc = range.state();
        if (failed || rc.failed || rc.finished) {
            throw EntropyError("selector 3 decoder is closed or failed");
        }
        const auto count = (std::min)(requested, static_cast<std::size_t>(total - decoded));
        if (count > kMaxEntropyBatchSamples) {
            throw UnsupportedCodec("selector 3 batch exceeds the allocation limit; split into frames");
        }
        if (count % channels) {
            throw EntropyError("selector 3 batch has a partial frame");
        }
        std::vector<std::int32_t> output(count);
        try {
            for (std::size_t frame = 0; frame < count; frame += channels) {
                for (unsigned channel = 0; channel < channels; ++channel) {
                    output[frame + channel] = decode_one(state[channel]);
                }
            }
            decoded += static_cast<std::uint32_t>(count);
            if (decoded == total) {
                // Same lifetime boundary as 100052e3..10005343. Unlike native
                // delete[] bookkeeping, RAII also cleans up truncated blocks.
                for (auto &channel : state) {
                    channel.models.clear();
                    channel.scale_models.clear();
                }
            }
        } catch (...) {
            failed = true; // never continue partially updated arithmetic state
            throw;
        }
        return output;
    }
};

EntropySelector3::EntropySelector3(RangeDecoder &range, unsigned bit_width,
    unsigned channels, std::uint32_t total_scalar_samples)
    : impl_(std::make_unique<Impl>(range, bit_width, channels, total_scalar_samples)) {}

EntropySelector3::~EntropySelector3() = default;

std::vector<std::int32_t> EntropySelector3::decode(std::size_t scalar_count) {
    return impl_->decode(scalar_count);
}

} // namespace openfrog
