#include "predictor.hpp"

#include "entropy.hpp"
#include "predictor_cascade.hpp"
#include "predictor_lpc.hpp"

#include <algorithm>
#include <array>
#include <cfenv>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#if defined(_M_IX86) || defined(_M_X64) || defined(__SSE2__)
#include <xmmintrin.h>
#endif

#if defined(__FAST_MATH__) || (defined(_M_FP_FAST) && _M_FP_FAST)
#error OptimFROG prediction requires strict IEEE floating-point arithmetic.
#endif

namespace openfrog {
namespace {

static_assert(sizeof(double) == 8 && sizeof(float) == 4
    && std::numeric_limits<double>::is_iec559
    && std::numeric_limits<float>::is_iec559,
    "OptimFROG prediction requires IEEE binary32 and binary64");

double div64(double a, double b) {
    volatile double value = a / b;
    return value;
}

void require_nearest() {
    if (std::fegetround() != FE_TONEAREST) {
        throw UnsupportedCodec("predictor requires round-to-nearest arithmetic");
    }
#if defined(_M_IX86) || defined(_M_X64) || defined(__SSE2__)
    if ((_mm_getcsr() & 0xe040U) != 0) {
        throw UnsupportedCodec("predictor requires nearest SSE arithmetic without FTZ/DAZ");
    }
#endif
    // Check binary32 as well as binary64: the adaptive cascade uses both.
    volatile double d = std::numeric_limits<double>::denorm_min();
    volatile double twice_d = d + d;
    const double stored_d = twice_d;
    std::uint64_t dbits = 0;
    std::memcpy(&dbits, &stored_d, sizeof(dbits));
    volatile float f = std::numeric_limits<float>::denorm_min();
    volatile float twice_f = f + f;
    const float stored_f = twice_f;
    std::uint32_t fbits = 0;
    std::memcpy(&fbits, &stored_f, sizeof(fbits));
    if (dbits != 2 || fbits != 2) {
        throw UnsupportedCodec("predictor requires gradual underflow (no FTZ/DAZ)");
    }
}

std::int32_t signed32(std::uint32_t bits) noexcept {
    const auto wide = bits < UINT32_C(0x80000000)
        ? static_cast<std::int64_t>(bits)
        : static_cast<std::int64_t>(bits) - INT64_C(0x100000000);
    return static_cast<std::int32_t>(wide);
}

// CVTSD2SI with the initialized MXCSR (nearest, ties-to-even), including the
// integer-indefinite result for NaN/infinity/out-of-range. std::round is wrong.
std::int32_t nearest_integer(double value) noexcept {
    if (!std::isfinite(value)) {
        return (std::numeric_limits<std::int32_t>::min)();
    }
    const double rounded = std::nearbyint(value);
    if (rounded < -2147483648.0 || rounded >= 2147483648.0) {
        return (std::numeric_limits<std::int32_t>::min)();
    }
    return static_cast<std::int32_t>(rounded);
}

std::int32_t sign_extend(std::uint32_t bits, unsigned width) noexcept {
    if (width == 32) {
        return signed32(bits);
    }
    const auto sign = std::uint32_t(1) << (width - 1);
    bits &= (sign << 1) - 1U;
    return signed32((bits ^ sign) - sign);
}

void require_spec(const PredictorSpec &spec, std::uint32_t count) {
    if (spec.channels < 1 || spec.channels > 2 || spec.bit_width < 1
        || spec.bit_width > 32 || count % spec.channels) {
        throw PredictorError("invalid predictor channels, width or sample count");
    }
    const std::int64_t limit = INT64_C(1) << (spec.bit_width - 1);
    for (unsigned channel = 0; channel < spec.channels; ++channel) {
        if (spec.minimum[channel] > spec.maximum[channel]
            || spec.minimum[channel] < -limit
            || spec.maximum[channel] >= limit) {
            throw PredictorError("predictor bounds exceed reduced signed width");
        }
    }
}

// 10005b70 / 100062e0: two binary contexts per channel. The first context is
// zero; subsequent symbols are conditioned on the previous switch value.
struct SwitchingTable {
    std::array<std::vector<std::uint8_t>, 2> modes;
    std::uint32_t remaining = 0;
    std::uint32_t period = 0;
    std::size_t index = 0;

    SwitchingTable(RangeDecoder &range, unsigned channels, std::uint32_t frames,
        const predictor_detail::LpcParameters &parameters)
        : remaining((std::min)(frames,
              static_cast<std::uint32_t>(parameters.order
                  - (channels == 2 ? parameters.cross_order : 0) + 1))),
          period(parameters.period) {
        if (!period) {
            throw PredictorError("zero predictor switching period");
        }
        const auto rest = frames - remaining;
        const auto later = rest / period + (rest % period != 0);
        // An explicit resource bound, not a claim about the format's limits.
        if (later > 16U * 1024U * 1024U) {
            throw UnsupportedCodec("predictor switching table exceeds allocation budget");
        }
        std::array<std::array<AdaptiveTree, 2>, 2> trees{{
            {{AdaptiveTree(2, 0x8000), AdaptiveTree(2, 0x8000)}},
            {{AdaptiveTree(2, 0x8000), AdaptiveTree(2, 0x8000)}}}};
        std::array<unsigned, 2> context{{0, 0}};
        for (unsigned channel = 0; channel < channels; ++channel) {
            modes[channel].reserve(static_cast<std::size_t>(later) + 2);
            context[channel] = trees[channel][0].decode(range);
            modes[channel].push_back(static_cast<std::uint8_t>(context[channel]));
        }
        for (std::uint32_t i = 0; i < later; ++i) {
            for (unsigned channel = 0; channel < channels; ++channel) {
                context[channel] = trees[channel][context[channel]].decode(range);
                modes[channel].push_back(static_cast<std::uint8_t>(context[channel]));
            }
        }
        for (unsigned channel = 0; channel < channels; ++channel) {
            modes[channel].push_back(1); // native end sentinel suppresses solve
        }
    }

    unsigned current(unsigned channel) const { return modes[channel][index]; }
    bool suppress_next(unsigned channel) const {
        return modes[channel][index + 1] == 1;
    }
    void advance() {
        if (--remaining == 0) {
            ++index;
            remaining = period;
        }
    }
};

std::int32_t read_scale_delta(RangeDecoder &range, AdaptiveTree &tree) {
    const unsigned exponent = tree.decode(range);
    const auto base = exponent ? std::uint32_t(1) << exponent : 0U;
    const auto tail = range.bits(exponent ? exponent : 1U);
    return unfold(base + tail);
}

} // namespace

struct Predictor::Impl {
    unsigned selector;
    PredictorSpec spec;
    std::uint32_t total;
    std::uint32_t consumed = 0;
    RangeDecoder *correction;
    bool modern_correction = false;
    bool failed = false;
    std::unique_ptr<predictor_detail::LpcPredictor> lpc;
    std::unique_ptr<predictor_detail::CascadePredictor> cascade;
    std::unique_ptr<SwitchingTable> switching;
    std::uint32_t scale_period = 0;
    std::array<std::vector<std::int32_t>, 2> scales;
    std::array<std::unique_ptr<predictor_detail::SmallMixer>, 2> correction_mixer;
    std::array<std::array<double, 8>, 2> correction_history{};
    std::unique_ptr<AdaptiveTree> correction_tree;

    Impl(unsigned selected, const PredictorSpec &bounds, std::uint32_t count,
        RangeDecoder &main_range, RangeDecoder *extra)
        : selector(selected), spec(bounds), total(count), correction(extra) {
        require_spec(spec, total);
        require_nearest();
        if (selector < 1 || selector > 4) {
            throw UnsupportedCodec("unsupported predictor selector");
        }
        if (correction && selector != 4) {
            throw UnsupportedCodec("correction stream requires predictor selector 4");
        }
        if (correction) {
            const auto tag = correction->state().tag;
            if (tag != 0x10 && tag != 0x11) {
                throw UnsupportedCodec("unsupported correction range tag");
            }
            modern_correction = tag == 0x11;
        }
        predictor_detail::LpcParameters parameters{};
        if (selector != 2) {
            parameters = predictor_detail::read_lpc_parameters(main_range, spec.channels);
            lpc = std::make_unique<predictor_detail::LpcPredictor>(parameters, spec.channels);
        }
        if (selector == 2 || selector == 3) {
            const auto adaptive = predictor_detail::read_cascade_parameters(
                main_range, spec.channels);
            cascade = std::make_unique<predictor_detail::CascadePredictor>(adaptive, spec);
        }
        const auto frames = total / spec.channels;
        if (selector == 3) {
            switching = std::make_unique<SwitchingTable>(main_range, spec.channels,
                frames, parameters);
        }
        if (selector == 4) {
            scale_period = main_range.smallBits(12) + 1;
            const auto segments = frames / scale_period + (frames % scale_period != 0);
            if (segments > 16U * 1024U * 1024U) {
                throw UnsupportedCodec("predictor scale table exceeds allocation budget");
            }
            AdaptiveTree model(32, 0x8000);
            std::array<std::uint32_t, 2> previous{{1, 1}}, before_previous{{1, 1}};
            for (unsigned channel = 0; channel < spec.channels; ++channel) {
                scales[channel].reserve(segments);
            }
            for (std::uint32_t segment = 0; segment < segments; ++segment) {
                for (unsigned channel = 0; channel < spec.channels; ++channel) {
                    const auto delta = read_scale_delta(main_range, model);
                    const auto current = 2U * previous[channel] - before_previous[channel]
                        + static_cast<std::uint32_t>(delta);
                    const auto scale = signed32(current);
                    if (scale <= 0) {
                        throw PredictorError("nonpositive predictor scale");
                    }
                    scales[channel].push_back(scale);
                    before_previous[channel] = previous[channel];
                    previous[channel] = current;
                }
            }
            if (modern_correction) {
                // The original constant at VA10023d50, represented exactly.
                std::uint64_t alpha_bits = UINT64_C(0x3fefeaaaaaaaaaab);
                double alpha = 0;
                std::memcpy(&alpha, &alpha_bits, sizeof(alpha));
                for (unsigned channel = 0; channel < spec.channels; ++channel) {
                    correction_mixer[channel] =
                        std::make_unique<predictor_detail::SmallMixer>(
                            2, scale_period, alpha, scale_period);
                }
                correction_tree = std::make_unique<AdaptiveTree>(16, 0x8000);
            }
        }
    }

    std::int32_t conventional(double prediction, std::int32_t residual,
        unsigned channel) const {
        auto integer = nearest_integer(prediction);
        integer = (std::max)(spec.minimum[channel],
            (std::min)(integer, spec.maximum[channel]));
        const auto sample = sign_extend(static_cast<std::uint32_t>(integer)
            + static_cast<std::uint32_t>(residual), spec.bit_width);
        if (sample < spec.minimum[channel] || sample > spec.maximum[channel]) {
            throw PredictorError("reconstructed predictor sample outside reduced bounds");
        }
        return sample;
    }

    std::int32_t correction_sample(unsigned channel, std::int32_t scale,
        std::int32_t quotient) {
        if (!correction) {
            return 0;
        }
        const auto unsigned_scale = static_cast<std::uint32_t>(scale);
        if (!modern_correction) {
            // 100080e0 / 10008690. Even scales have a tie-to-even dependent
            // alphabet: odd quotients exclude both half-step endpoints.
            if (unsigned_scale & 1U) {
                return scale == 1 ? 0
                    : signed32(correction->uniform(unsigned_scale) - (unsigned_scale >> 1));
            }
            if ((static_cast<std::uint32_t>(quotient) & 1U) == 0) {
                return signed32(correction->uniform(unsigned_scale + 1U)
                    - (unsigned_scale >> 1));
            }
            return scale <= 3 ? 0 : signed32(correction->uniform(unsigned_scale - 1U)
                - (unsigned_scale >> 1) + 1U);
        }
        const auto prediction = nearest_integer(
            correction_mixer[channel]->predict(correction_history[channel]));
        std::int32_t result = 0;
        if (scale != 1) {
            auto group = correction_tree->decode(*correction);
            if (group == 15) {
                const auto exponent = correction->smallBits(5);
                const auto base = exponent ? std::uint32_t(1) << exponent : 0U;
                group += base + correction->bits(exponent ? exponent : 1U);
            }
            const auto alphabet = unsigned_scale | 1U;
            const auto folded = correction->uniform(alphabet) + group * alphabet;
            result = signed32(static_cast<std::uint32_t>(prediction)
                + static_cast<std::uint32_t>(unfold(folded)));
        }
        correction_mixer[channel]->update(correction_history[channel],
            static_cast<double>(result));
        // Native history advances BEFORE prediction and has two leading old
        // values. A newest-first array gives the same param_2[-1],[-2] view.
        correction_history[channel][1] = correction_history[channel][0];
        correction_history[channel][0] = static_cast<double>(result);
        return result;
    }

    std::int32_t reconstruct_one(std::int32_t residual, unsigned channel) {
        if (selector == 4) {
            const auto frame = consumed / spec.channels;
            const auto scale = scales[channel][frame / scale_period];
            const auto quotient = sign_extend(static_cast<std::uint32_t>(nearest_integer(
                div64(lpc->predict(channel), static_cast<double>(scale))))
                + static_cast<std::uint32_t>(residual), spec.bit_width);
            const auto main_sample = signed32(static_cast<std::uint32_t>(scale)
                * static_cast<std::uint32_t>(quotient));
            // Training deliberately precedes correction and never sees it.
            lpc->update(channel, static_cast<double>(main_sample));
            const auto adjustment = correction_sample(channel, scale, quotient);
            return signed32(static_cast<std::uint32_t>(main_sample)
                + static_cast<std::uint32_t>(adjustment));
        }
        if (selector == 1) {
            const auto sample = conventional(lpc->predict(channel), residual, channel);
            lpc->update(channel, static_cast<double>(sample));
            return sample;
        }
        if (selector == 2) {
            const auto sample = conventional(cascade->predict(channel), residual, channel);
            cascade->update(channel, static_cast<double>(sample));
            return sample;
        }
        if (switching->current(channel) == 0) {
            const auto sample = conventional(lpc->predict(channel), residual, channel);
            lpc->update(channel, static_cast<double>(sample));
            (void)cascade->predict(channel);
            cascade->update(channel, static_cast<double>(sample));
            return sample;
        }
        const auto sample = conventional(cascade->predict(channel), residual, channel);
        cascade->update(channel, static_cast<double>(sample));
        lpc->update(channel, static_cast<double>(sample));
        return sample;
    }
};

Predictor::Predictor(unsigned selector, const PredictorSpec &spec,
    std::uint32_t total_scalar_samples, RangeDecoder &main_range,
    RangeDecoder *correction)
    : impl_(std::make_unique<Impl>(selector, spec, total_scalar_samples,
          main_range, correction)) {}

Predictor::~Predictor() = default;

void Predictor::reconstruct(std::int32_t *residuals, std::size_t scalar_count) {
    auto &state = *impl_;
    if (state.failed) {
        throw PredictorError("predictor is failed");
    }
    if ((scalar_count && !residuals) || scalar_count % state.spec.channels
        || scalar_count > state.total - state.consumed) {
        throw PredictorError("invalid predictor batch or exhausted block");
    }
    require_nearest();
    if (state.correction) {
        const auto range_state = state.correction->state();
        if (range_state.finished || range_state.failed) {
            throw PredictorError("correction range decoder is closed or failed");
        }
    }
    try {
        for (std::size_t i = 0; i < scalar_count; i += state.spec.channels) {
            if (state.switching && state.switching->remaining == 1) {
                for (unsigned channel = 0; channel < state.spec.channels; ++channel) {
                    state.lpc->suppress_recalculation(channel,
                        state.switching->suppress_next(channel));
                }
            }
            for (unsigned channel = 0; channel < state.spec.channels; ++channel) {
                residuals[i + channel] = state.reconstruct_one(residuals[i + channel], channel);
            }
            state.consumed += state.spec.channels;
            if (state.switching) {
                state.switching->advance();
            }
        }
    } catch (...) {
        state.failed = true;
        throw;
    }
}

} // namespace openfrog
