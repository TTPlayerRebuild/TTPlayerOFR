#include "predictor_cascade.hpp"

#include <algorithm>
#include <cfenv>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#if defined(_M_IX86) || defined(_M_X64) || defined(__SSE2__)
#include <xmmintrin.h>
#endif

#if defined(__FAST_MATH__) || (defined(_M_FP_FAST) && _M_FP_FAST)
#error OptimFROG cascade requires strict IEEE floating-point arithmetic.
#endif

namespace openfrog::predictor_detail {
namespace {

// E1 ev_1d00946de146ebfa7ac105c17ce72e12695784dbb51498539c0e632edad806f0:
// 06d30/07160, 09250..09ef2, 0f6c0..10037. Initialization pointer walks are
// checked against supplementary-ghidra/0x10009910.asm and 0x10009a20.asm.
// Every operation below has the storage precision of the corresponding SSE
// instruction. In particular neither a float multiply-add nor LDL subtraction
// may contract into FMA, or accumulate in an extended-precision register.
static_assert(std::numeric_limits<float>::is_iec559 && sizeof(float) == 4,
    "cascade requires IEEE binary32");
static_assert(std::numeric_limits<double>::is_iec559 && sizeof(double) == 8,
    "cascade requires IEEE binary64");

double add(double a, double b) { volatile double v = a + b; return v; }
double sub(double a, double b) { volatile double v = a - b; return v; }
double mul(double a, double b) { volatile double v = a * b; return v; }
double div(double a, double b) { volatile double v = a / b; return v; }
float add32(float a, float b) { volatile float v = a + b; return v; }
float mul32(float a, float b) { volatile float v = a * b; return v; }
float to32(double a) { volatile float v = static_cast<float>(a); return v; }

void require_float_environment() {
    if (std::fegetround() != FE_TONEAREST) {
        throw UnsupportedCodec("cascade requires round-to-nearest floating arithmetic");
    }
#if defined(_M_IX86) || defined(_M_X64) || defined(__SSE2__)
    // x87 fegetround() need not reflect the independent SSE rounding control.
    if ((_mm_getcsr() & 0xe040U) != 0) {
        throw UnsupportedCodec("cascade requires nearest SSE arithmetic without FTZ/DAZ");
    }
#endif
    volatile float tiny32 = std::numeric_limits<float>::denorm_min();
    volatile float twice32 = tiny32 + tiny32;
    volatile double tiny64 = std::numeric_limits<double>::denorm_min();
    volatile double twice64 = tiny64 + tiny64;
    const float stored32 = twice32;
    const double stored64 = twice64;
    std::uint32_t bits32;
    std::uint64_t bits64;
    std::memcpy(&bits32, &stored32, sizeof(bits32));
    std::memcpy(&bits64, &stored64, sizeof(bits64));
    if (bits32 != 2 || bits64 != 2) {
        throw UnsupportedCodec("cascade requires gradual floating-point underflow");
    }
}

using Vector = std::array<double, 8>;
using Matrix = std::array<Vector, 8>;

// 1000f6c0: lower-triangle LDL^T, no pivoting. The multiplication order,
// sequential subtractions, and division before back-substitution are material.
bool solve(Matrix &a, Vector &b, unsigned n) {
    Vector diagonal{};
    for (unsigned i = 0; i < n; ++i) {
        for (unsigned j = 0; j < i; ++j) {
            double value = a[i][j];
            for (unsigned k = 0; k < j; ++k) {
                value = sub(value, mul(mul(diagonal[k], a[i][k]), a[j][k]));
            }
            a[i][j] = div(value, diagonal[j]);
        }
        double value = a[i][i];
        a[i][i] = 1;
        for (unsigned k = 0; k < i; ++k) {
            value = sub(value, mul(mul(a[i][k], a[i][k]), diagonal[k]));
        }
        diagonal[i] = value;
        if (std::abs(value) < 0x1p-17) { return false; }
    }
    for (unsigned i = 1; i < n; ++i) {
        double value = b[i];
        for (unsigned j = 0; j < i; ++j) {
            value = sub(value, mul(b[j], a[i][j]));
        }
        b[i] = value;
    }
    for (unsigned index = n; index != 0; --index) {
        const unsigned i = index - 1;
        double value = div(b[i], diagonal[i]);
        for (unsigned j = i + 1; j < n; ++j) {
            value = sub(value, mul(b[j], a[j][i]));
        }
        b[i] = value;
    }
    return true;
}

// These are the data objects, not the pointers that follow them in .data.
// 1002911c -> file 2711c: eight own lengths; 10029124 -> file 27124: four
// cross lengths, aliasing the final four entries of the first table.
constexpr std::array<std::uint32_t, 8> own_lengths{{512, 256, 128, 64, 128, 64, 32, 16}};
constexpr std::array<std::uint32_t, 4> cross_lengths{{128, 64, 32, 16}};

void validate_lengths(std::uint32_t own, std::uint32_t cross, unsigned channels) {
    // These maxima follow the escape-byte syntax, not an allocation budget:
    // own = (byte + 1) * 4; cross = byte * 4. Alignment is checked natively.
    if (own == 0 || own > 1024 || (own != 4 && (own & 7U) != 0)) {
        throw EntropyError("invalid cascade filter length");
    }
    if (channels == 2 && (cross > 1020
        || (((own & 7U) != 0 || (cross & 7U) != 0) && (own != 4 || cross != 4)))) {
        throw EntropyError("invalid stereo cascade filter lengths");
    }
}

struct History {
    std::vector<float> samples;
    std::size_t retained;
    std::size_t capacity;
    std::size_t current;

    explicit History(std::size_t keep)
        : samples(std::max<std::size_t>(1024, keep + 2) + 8, 0),
          retained(keep), capacity(samples.size() - 8), current(keep) {}

    void advance() {
        ++current;
        if (current == capacity) {
            // Native 09510/09610/09710 preserve the oldest energy term too.
            for (std::size_t i = 0; i < retained; ++i) {
                samples[i] = samples[capacity - retained + i];
            }
            current = retained;
        }
        samples[current] = 0;
    }
    float *at() { return samples.data() + current; }
    const float *at() const { return samples.data() + current; }
};

struct Filter {
    std::uint32_t own;
    std::uint32_t cross;
    std::vector<float> own_weights;
    std::vector<float> cross_weights;
    double energy = 0;
    double mu;
    double regularizer;
    History history;

    Filter(const CascadeStageParameters &p, double amplitude, unsigned channels)
        : own(p.taps), cross(channels == 2 ? p.cross_taps : 0),
          own_weights(std::max<std::uint32_t>(8, (own + 7U) & ~7U), 0),
          cross_weights(channels == 2 ? std::max<std::uint32_t>(8, (cross + 7U) & ~7U) : 0, 0),
          mu(p.mu), regularizer(amplitude),
          history(std::max<std::uint32_t>(own + 1U, cross)) {}

    static void dot_lanes(std::array<float, 4> &acc, const std::vector<float> &weights,
        const float *inputs) {
        for (std::size_t base = 0; base < weights.size(); base += 8) {
            for (unsigned lane = 0; lane < 4; ++lane) {
                const float first = mul32(inputs[base + lane], weights[base + lane]);
                const float partial = add32(first, acc[lane]);
                const float second = mul32(inputs[base + 4 + lane], weights[base + 4 + lane]);
                acc[lane] = add32(second, partial);
            }
        }
    }

    double predict(const History *other) const {
        std::array<float, 4> acc{};
        dot_lanes(acc, own_weights, history.at() - own);
        if (other) { dot_lanes(acc, cross_weights, other->at() - cross + 1); }
        // SHUFPS D8/4E, CVTPS2PD, ADDPD, ADDSD: not a float horizontal sum.
        return add(add(static_cast<double>(acc[1]), static_cast<double>(acc[0])),
            add(static_cast<double>(acc[3]), static_cast<double>(acc[2])));
    }

    static void adapt(std::vector<float> &weights, const float *inputs, float gain) {
        for (std::size_t j = 0; j < weights.size(); ++j) {
            weights[j] = add32(mul32(inputs[j], gain), weights[j]);
        }
    }

    void update(const History *other, float error, double input) {
        const float *own_inputs = history.at() - own;
        const double leaving = static_cast<double>(own_inputs[-1]);
        energy = sub(energy, mul(leaving, leaving));
        const double entering = static_cast<double>(history.at()[-1]);
        energy = add(mul(entering, entering), energy);
        if (other) {
            const double cross_leaving = static_cast<double>(other->at()[-static_cast<std::ptrdiff_t>(cross)]);
            energy = sub(energy, mul(cross_leaving, cross_leaving));
            const double cross_entering = static_cast<double>(*other->at());
            energy = add(mul(cross_entering, cross_entering), energy);
        }
        const float gain = to32(div(mul(static_cast<double>(error), mu), add(regularizer, energy)));
        adapt(own_weights, own_inputs, gain);
        if (other) { adapt(cross_weights, other->at() - cross + 1, gain); }
        // The native stereo special case keys off the second loop's remainder.
        if ((other ? cross : own) == 4) {
            std::fill(own_weights.begin() + 4, own_weights.end(), 0.0F);
            if (other) { std::fill(cross_weights.begin() + 4, cross_weights.end(), 0.0F); }
        }
        *history.at() = to32(input);
    }
};

} // namespace

struct SmallMixer::Impl {
    unsigned dimensions;
    std::uint32_t period;
    double alpha;
    std::uint32_t counter = 0;
    std::uint32_t last_update;
    Matrix covariance{};
    std::array<double, 9> target_products{}; // target^2, then target * input[i]
    Vector weights{{1, 0, 0, 0, 0, 0, 0, 0}};

    Impl(unsigned n, std::uint32_t p, double a, std::uint32_t initial)
        : dimensions(n), period(p), alpha(a), last_update(initial) {
        require_float_environment();
        if (n > 8) {
            throw UnsupportedCodec("small covariance mixer supports at most eight dimensions");
        }
        if (n == 0 || !std::isfinite(a) || a < 0 || a > 1) {
            throw EntropyError("invalid small covariance mixer parameters");
        }
    }

    void update(const Vector &inputs, double target) {
        ++counter;
        for (unsigned i = 0; i < dimensions; ++i) {
            for (unsigned j = 0; j <= i; ++j) {
                covariance[i][j] = add(mul(inputs[i], inputs[j]), mul(alpha, covariance[i][j]));
            }
        }
        target_products[0] = add(mul(target, target), mul(alpha, target_products[0]));
        for (unsigned i = 0; i < dimensions; ++i) {
            target_products[i + 1] = add(mul(inputs[i], target), mul(alpha, target_products[i + 1]));
        }
        if (counter < static_cast<std::uint32_t>(period + last_update)) { return; }
        const double energy = target_products[0];
        if (energy >= 0.5) {
            Matrix matrix = covariance;
            for (unsigned i = 0; i < dimensions; ++i) { weights[i] = target_products[i + 1]; }
            if (!solve(matrix, weights, dimensions)) {
                std::fill(weights.begin(), weights.end(), 0.0);
                unsigned selected = 0;
                for (unsigned i = 0; i < dimensions; ++i) {
                    if (energy == target_products[i + 1]) { selected = i; break; }
                }
                weights[selected] = 1;
            }
        } else {
            if (energy != 0 && energy < 0x1p-30) {
                for (unsigned i = 0; i < dimensions; ++i) {
                    for (unsigned j = 0; j <= i; ++j) { covariance[i][j] = 0; }
                }
                target_products.fill(0);
            }
            weights.fill(0);
            weights[0] = 1;
        }
        last_update = counter;
    }
};

SmallMixer::SmallMixer(unsigned dimensions, std::uint32_t period, double alpha,
    std::uint32_t initial_counter)
    : impl_(std::make_unique<Impl>(dimensions, period, alpha, initial_counter)) {}
SmallMixer::~SmallMixer() = default;
SmallMixer::SmallMixer(SmallMixer &&) noexcept = default;
SmallMixer &SmallMixer::operator=(SmallMixer &&) noexcept = default;

double SmallMixer::predict(const Vector &inputs) const {
    double result = 0;
    // 1000faf0 has scalar MULSD/ADDSD, in nearest-to-farthest order, including
    // its unrolled groups of four (there is no pairwise horizontal reduction).
    for (unsigned i = 0; i < impl_->dimensions; ++i) {
        result = add(mul(inputs[i], impl_->weights[i]), result);
    }
    return result;
}

void SmallMixer::update(const Vector &inputs, double target) { impl_->update(inputs, target); }

CascadeParameters read_cascade_parameters(RangeDecoder &range, unsigned channels) {
    if (channels != 1 && channels != 2) { throw EntropyError("invalid cascade channel count"); }
    CascadeParameters p;
    p.stages = range.smallBits(3) + 1;
    if (range.smallBits(1) != 0) { p.preemphasis = mul(static_cast<double>(range.smallBits(10)), 0x1p-10); }
    const auto decay_code = range.smallBits(12);
    p.decay = decay_code == 4095 ? range.u16() + 4097 : decay_code + 2;
    p.period = std::uint32_t{1} << range.smallBits(3);
    if (range.smallBits(1) != 0) { p.initial_counter = range.u16(); }
    for (unsigned i = 0; i < p.stages; ++i) {
        const auto selector = range.smallBits(5);
        auto &filter = p.filters[i];
        if (selector == 31) {
            filter.taps = (range.byteSymbol() + 1) * 4;
            if (channels == 2) { filter.cross_taps = range.byteSymbol() * 4; }
        } else {
            if (selector >= own_lengths.size() || (channels == 2 && selector >= cross_lengths.size())) {
                // The valid domain beyond the verified table prefix is unknown;
                // do not conceal an unimplemented mode as damaged audio.
                throw UnsupportedCodec("unverified cascade length-table selector");
            }
            filter.taps = own_lengths[selector];
            filter.cross_taps = channels == 2 ? cross_lengths[selector] : 0;
        }
        filter.mu = div(static_cast<double>(range.smallBits(10)), 1000.0);
        validate_lengths(filter.taps, filter.cross_taps, channels);
    }
    return p;
}

struct CascadePredictor::Impl {
    struct Channel {
        SmallMixer mixer;
        std::vector<Filter> filters;
        Vector predictions{};
        Vector inputs{};
        double previous = 0;
        Channel(const CascadeParameters &p, double amplitude, unsigned channels)
            : mixer(p.stages, p.period, div(sub(static_cast<double>(p.decay), 1),
                static_cast<double>(p.decay)), p.initial_counter) {
            filters.reserve(p.stages);
            for (unsigned i = 0; i < p.stages; ++i) {
                filters.emplace_back(p.filters[i], amplitude, channels);
            }
        }
    };
    CascadeParameters parameters;
    unsigned channels;
    std::vector<Channel> state;
    unsigned next_channel = 0;
    bool pending = false;

    Impl(const CascadeParameters &p, const PredictorSpec &spec)
        : parameters(p), channels(spec.channels) {
        if (p.stages > 8) {
            throw UnsupportedCodec("cascade supports at most eight filter stages");
        }
        if ((channels != 1 && channels != 2) || p.stages == 0
            || p.decay < 2 || !std::isfinite(p.preemphasis)) {
            throw EntropyError("invalid cascade parameters");
        }
        double amplitude = 1;
        for (unsigned c = 0; c < channels; ++c) {
            if (spec.minimum[c] > spec.maximum[c]) { throw EntropyError("invalid predictor bounds"); }
            amplitude = std::max(amplitude, std::abs(static_cast<double>(spec.minimum[c])));
            amplitude = std::max(amplitude, std::abs(static_cast<double>(spec.maximum[c])));
        }
        for (unsigned i = 0; i < p.stages; ++i) {
            validate_lengths(p.filters[i].taps, p.filters[i].cross_taps, channels);
            if (!std::isfinite(p.filters[i].mu)) { throw EntropyError("invalid cascade adaptation gain"); }
        }
        state.reserve(channels);
        for (unsigned c = 0; c < channels; ++c) { state.emplace_back(p, amplitude, channels); }
    }

    double predict(unsigned channel) {
        if (channel != next_channel || pending) {
            throw UnsupportedCodec("cascade requires ordered predict/update pairs");
        }
        auto &s = state[channel];
        double cumulative = 0;
        for (unsigned i = 0; i < parameters.stages; ++i) {
            auto &filter = s.filters[i];
            filter.history.advance();
            const History *other = channels == 2 ? &state[1 - channel].filters[i].history : nullptr;
            s.predictions[i] = filter.predict(other);
            cumulative = add(s.predictions[i], cumulative);
            s.inputs[parameters.stages - i - 1] = cumulative;
        }
        pending = true;
        return add(s.mixer.predict(s.inputs), s.previous);
    }

    void update(unsigned channel, double sample) {
        if (channel != next_channel || !pending) {
            throw UnsupportedCodec("cascade update requires its preceding prediction");
        }
        auto &s = state[channel];
        const double target = sub(sample, s.previous);
        s.previous = mul(parameters.preemphasis, sample);
        double input = target;
        for (unsigned i = 0; i < parameters.stages; ++i) {
            const double error = sub(input, s.predictions[i]);
            const History *other = channels == 2 ? &state[1 - channel].filters[i].history : nullptr;
            s.filters[i].update(other, to32(error), input);
            input = error;
        }
        s.mixer.update(s.inputs, target);
        pending = false;
        next_channel = (channel + 1) % channels;
    }
};

CascadePredictor::CascadePredictor(const CascadeParameters &parameters, const PredictorSpec &spec)
    : impl_(std::make_unique<Impl>(parameters, spec)) {}
CascadePredictor::~CascadePredictor() = default;
CascadePredictor::CascadePredictor(CascadePredictor &&) noexcept = default;
CascadePredictor &CascadePredictor::operator=(CascadePredictor &&) noexcept = default;
double CascadePredictor::predict(unsigned channel) { return impl_->predict(channel); }
void CascadePredictor::update(unsigned channel, double sample) { impl_->update(channel, sample); }

} // namespace openfrog::predictor_detail
