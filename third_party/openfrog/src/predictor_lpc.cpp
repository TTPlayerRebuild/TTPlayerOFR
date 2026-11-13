#include "predictor_lpc.hpp"
#include "codec_types.hpp"
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

#if defined(_M_IX86) || defined(_M_X64) || defined(__SSE2__)
#include <xmmintrin.h>
#endif

#if defined(__FAST_MATH__) || (defined(_M_FP_FAST) && _M_FP_FAST)
#error OptimFROG LPC requires strict IEEE floating-point arithmetic.
#endif

namespace openfrog {
namespace predictor_detail {
namespace {

static_assert(sizeof(double) == sizeof(std::uint64_t)
    && std::numeric_limits<double>::is_iec559
    && std::numeric_limits<double>::digits == 53,
    "LPC requires IEEE-754 binary64 double");

// E1 ev_1d00946de146ebfa7ac105c17ce72e12695784dbb51498539c0e632edad806f0:
// 10005550/05800, c6f0, cb90/cc40/ccf0/d9b0, e690/e710/eb70,
// efe0/f050/f0a0/f4a0. SSE instruction grouping was additionally checked
// against the same SHA-256 PE's read-only bytes, not inferred from C sums.
// Every elementary operation is stored separately: no FMA, reassociation or
// extended-precision accumulator is interchangeable with the original SSE2.
double add64(double a, double b) {
    volatile double result = a + b;
    return result;
}

double sub64(double a, double b) {
    volatile double result = a - b;
    return result;
}

double mul64(double a, double b) {
    volatile double result = a * b;
    return result;
}

double div64(double a, double b) {
    volatile double result = a / b;
    return result;
}

void require_nearest() {
    if (std::fegetround() != FE_TONEAREST) {
        throw UnsupportedCodec("LPC requires round-to-nearest floating arithmetic");
    }
#if defined(_M_IX86) || defined(_M_X64) || defined(__SSE2__)
    // On x86 the x87 and SSE rounding controls may disagree. fegetround()
    // alone is not sufficient to establish the SSE2 arithmetic contract.
    if ((_mm_getcsr() & 0xe040U) != 0) {
        throw UnsupportedCodec("LPC requires nearest SSE arithmetic without FTZ/DAZ");
    }
#endif
    volatile double tiny = std::numeric_limits<double>::denorm_min();
    volatile double twice = tiny + tiny;
    const double stored = twice;
    std::uint64_t bits = 0;
    std::memcpy(&bits, &stored, sizeof(bits));
    if (bits != 2) {
        throw UnsupportedCodec("LPC requires gradual binary64 underflow (no FTZ/DAZ)");
    }
}

constexpr unsigned kMatrixStride = 128;
constexpr unsigned kHistoryLength = 1024;
constexpr unsigned kMonoCorrelations = 130;
constexpr unsigned kStereoCorrelations = 258;
constexpr double kPivotThreshold = 0x1p-17;
constexpr double kZeroThreshold = 0x1p-30;

unsigned round_up(unsigned value, unsigned alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

// Exponentiation-by-squaring, including the final (otherwise unused) square,
// rather than libm pow(). The initial covariance is omitted at count >= 48000.
double initial_weight(double alpha, std::uint32_t exponent) {
    double result = 1.0;
    while (exponent != 0) {
        if (exponent & 1U) {
            result = mul64(result, alpha);
        }
        exponent >>= 1;
        alpha = mul64(alpha, alpha);
    }
    return result;
}

// 1000c6f0: unpivoted LDL^T. In particular, D[k]*L[i,k] is rounded BEFORE
// multiplying L[j,k], and backward substitution divides before subtracting.
bool solve(double *matrix, double *rhs, double *diagonal, unsigned n) {
    diagonal[0] = matrix[0];
    if (std::fabs(diagonal[0]) < kPivotThreshold) {
        return false;
    }
    matrix[0] = 1.0;
    for (unsigned i = 1; i < n; ++i) {
        double *row = matrix + i * kMatrixStride;
        for (unsigned j = 0; j < i; ++j) {
            const double *other = matrix + j * kMatrixStride;
            double value = row[j];
            for (unsigned k = 0; k < j; ++k) {
                const double product = mul64(mul64(diagonal[k], row[k]), other[k]);
                value = sub64(value, product);
            }
            row[j] = div64(value, diagonal[j]);
        }
        double value = row[i];
        row[i] = 1.0;
        for (unsigned k = 0; k < i; ++k) {
            value = sub64(value, mul64(mul64(row[k], row[k]), diagonal[k]));
        }
        diagonal[i] = value;
        if (std::fabs(value) < kPivotThreshold) {
            return false;
        }
    }
    for (unsigned i = 1; i < n; ++i) {
        const double *row = matrix + i * kMatrixStride;
        double value = rhs[i];
        for (unsigned j = 0; j < i; ++j) {
            value = sub64(value, mul64(rhs[j], row[j]));
        }
        rhs[i] = value;
    }
    rhs[n - 1] = div64(rhs[n - 1], diagonal[n - 1]);
    for (unsigned i = n - 1; i-- > 0;) {
        double value = div64(rhs[i], diagonal[i]);
        for (unsigned j = i + 1; j < n; ++j) {
            value = sub64(value, mul64(rhs[j], matrix[j * kMatrixStride + i]));
        }
        rhs[i] = value;
    }
    return true;
}

} // namespace

LpcParameters read_lpc_parameters(RangeDecoder &range, unsigned channels) {
    if (channels < 1 || channels > 2) {
        throw EntropyError("LPC requires one or two channels");
    }
    // Direct PE .data reads: RVA 29000 -> file 27000 (NOT the .rdata map).
    // Only these nine entries at 29024/29038 are verified scalar order pairs.
    // The next entries overlap zeros, the neighboring table and pointers;
    // their format-level legality was not established by the static report.
    constexpr std::array<unsigned, 7> periods{{32, 64, 128, 256, 512, 1024, 2048}};
    constexpr std::array<unsigned, 9> orders{{12, 24, 36, 48, 64, 96, 128, 192, 256}};
    constexpr std::array<unsigned, 9> cross_orders{{4, 8, 12, 16, 20, 32, 40, 64, 80}};
    LpcParameters result;
    const auto decay_selector = range.smallBits(12);
    result.decay = decay_selector == 4095 ? range.u16() + 4097 : decay_selector + 2;
    const auto period_selector = range.smallBits(3);
    result.period = period_selector == 7 ? range.u16() + 1 : periods[period_selector];
    const auto order_selector = range.smallBits(5);
    if (order_selector == 31) {
        result.order = range.byteSymbol() + 1;
        if (channels == 2) {
            result.cross_order = range.byteSymbol();
        }
    } else {
        if (order_selector >= orders.size()) {
            throw UnsupportedCodec("unverified LPC order-table selector");
        }
        result.order = orders[order_selector];
        if (channels == 2) {
            result.cross_order = cross_orders[order_selector];
        }
    }
    return result;
}

struct LpcPredictor::Impl {
    LpcParameters parameters;
    unsigned channels;
    unsigned self_order;
    double alpha;
    std::array<std::uint32_t, 2> count{{0, 0}};
    std::array<std::uint32_t, 2> last_recalculation{{0, 0}};
    std::array<bool, 2> suppressed{{false, false}};
    std::array<unsigned, 2> history_position{{0, 0}};

    // Correlations, coefficient vectors and diagonal workspace have their
    // observed padding. Keeping the small adjacent arrays together also
    // preserves the native four-tap/two-lag tail behavior for escaped orders.
    std::vector<double> statistics;
    std::vector<double> history;
    std::array<std::vector<double>, 2> initial_covariance;
    std::vector<double> matrix;

    Impl(const LpcParameters &p, unsigned channel_count)
        : parameters(p), channels(channel_count),
          self_order(p.order - p.cross_order),
          alpha(0.0) {
        if (channels < 1 || channels > 2 || p.decay < 2 || p.decay > 69632
            || p.period == 0 || p.period > 65536 || p.order == 0
            || p.cross_order >= p.order
            || (channels == 1 && p.cross_order != 0)) {
            throw EntropyError("invalid LPC dimensions/parameters");
        }
        if (p.order > kMatrixStride) {
            // The shipped kernel passes order unchanged into fixed 128-wide
            // coefficient/LDL buffers. 192/256 table entries and >128 escapes
            // overrun those buffers; do not invent a different matrix kernel
            // or reproduce the original out-of-object writes in this product.
            throw UnsupportedCodec("LPC order exceeds the shipped 128-coefficient kernel");
        }
        if (p.cross_order > self_order) {
            // Native history retention is based on self_order, not total
            // order. A longer cross window can read beyond that channel's
            // history into the following object's fields. This is an unsafe
            // unsupported shape, not an asserted format-reserved parameter.
            throw UnsupportedCodec("LPC cross history exceeds retained self history");
        }
        require_nearest();
        alpha = div64(sub64(static_cast<double>(p.decay), 1.0),
            static_cast<double>(p.decay));
        if (channels == 1) {
            statistics.assign(kMonoCorrelations + 2 * kMatrixStride + 4, 0.0);
        } else {
            statistics.assign(2 * kStereoCorrelations + 3 * kMatrixStride + 4, 0.0);
        }
        history.assign(channels * kHistoryLength + 4, 0.0);
        for (unsigned channel = 0; channel < channels; ++channel) {
            history_position[channel] = kHistoryLength - 1 - self_order;
            coefficients(channel)[0] = 1.0;
            initial_covariance[channel].assign(kMatrixStride * kMatrixStride, 0.0);
        }
        matrix.assign(kMatrixStride * kMatrixStride, 0.0);
    }

    void require_channel(unsigned channel) const {
        if (channel >= channels) {
            throw EntropyError("LPC channel index out of range");
        }
    }

    double *correlations(unsigned channel) {
        return statistics.data() + (channels == 1 ? 0 : channel * kStereoCorrelations);
    }

    double *coefficients(unsigned channel) {
        return statistics.data() + (channels == 1 ? kMonoCorrelations
            : 2 * kStereoCorrelations + channel * kMatrixStride);
    }

    double *diagonal() {
        return statistics.data() + (channels == 1 ? kMonoCorrelations + kMatrixStride
            : 2 * kStereoCorrelations + 2 * kMatrixStride);
    }

    double *samples(unsigned channel) {
        return history.data() + channel * kHistoryLength + history_position[channel];
    }

    void set_suppressed(unsigned channel, bool suppress) {
        require_channel(channel);
        suppressed[channel] = suppress;
        // The terminal history guard occupies the same double as the native
        // suppression byte(s). It is normally outside all active taps, but a
        // padded escaped-order tail can read it at the history wrap boundary.
        const std::uint64_t bits = static_cast<std::uint64_t>(suppressed[0])
            | (static_cast<std::uint64_t>(suppressed[1]) << 8);
        std::memcpy(&history[channels * kHistoryLength], &bits, sizeof(bits));
    }

    void insert(unsigned channel, double sample) {
        unsigned &position = history_position[channel];
        const unsigned base = channel * kHistoryLength;
        if (position == 0) {
            for (unsigned i = 0; i < self_order; ++i) {
                history[base + kHistoryLength - self_order + i] = history[base + i];
            }
            position = kHistoryLength - self_order;
        }
        --position;
        history[base + position] = sample;
        ++count[channel];
    }

    double predict(unsigned channel) {
        require_channel(channel);
        const double *coefficient = coefficients(channel);
        double accumulator0 = 0.0;
        double accumulator1 = 0.0;
        const auto accumulate = [&](const double *values, unsigned length,
                                    unsigned &coefficient_index) {
            // Native do/while: even a zero cross-order executes one group.
            const unsigned padded = (std::max)(4U, round_up(length, 4));
            for (unsigned i = 0; i < padded; i += 4) {
                const double pair0 = add64(mul64(values[i + 1], coefficient[coefficient_index + 1]),
                    mul64(values[i], coefficient[coefficient_index]));
                const double pair1 = add64(mul64(values[i + 3], coefficient[coefficient_index + 3]),
                    mul64(values[i + 2], coefficient[coefficient_index + 2]));
                accumulator0 = add64(accumulator0, pair0);
                accumulator1 = add64(accumulator1, pair1);
                coefficient_index += 4;
            }
        };
        unsigned index = 0;
        accumulate(samples(channel), self_order, index);
        if (channels == 2) {
            accumulate(samples(1 - channel), parameters.cross_order, index);
        }
        return add64(accumulator0, accumulator1);
    }

    void seed(unsigned channel) {
        double *covariance = initial_covariance[channel].data();
        const double *self = samples(channel);
        for (unsigned i = 0; i < self_order; ++i) {
            for (unsigned j = 0; j <= i; ++j) {
                covariance[i * kMatrixStride + j] = mul64(self[j + 1], self[i + 1]);
            }
        }
        if (channels == 2) {
            const double *other = samples(1 - channel);
            // The self_order row/column is supplied directly by correlations
            // when rebuilding. It is intentionally not seeded here.
            for (unsigned i = self_order + 1; i < parameters.order; ++i) {
                for (unsigned j = 0; j < self_order; ++j) {
                    covariance[i * kMatrixStride + j] = mul64(other[i - self_order], self[j + 1]);
                }
                for (unsigned j = self_order + 1; j <= i; ++j) {
                    covariance[i * kMatrixStride + j] = mul64(other[j - self_order], other[i - self_order]);
                }
            }
        }
        last_recalculation[channel] = count[channel];
    }

    void update_correlations(unsigned channel) {
        double *r = correlations(channel);
        const double *self = samples(channel);
        const double current = self[0];
        if (channels == 1) {
            const unsigned end = round_up(self_order, 2);
            for (unsigned i = 0; i <= end; ++i) {
                r[i] = add64(mul64(r[i], alpha), mul64(self[i], current));
            }
            return;
        }
        const double *other = samples(1 - channel);
        const double other_current = other[0];
        const unsigned self_end = round_up(self_order, 2);
        unsigned index = 0;
        double previous = current;
        for (unsigned i = 0; i < self_end; ++i) {
            const double next = self[i + 1];
            r[index] = add64(mul64(r[index], alpha), mul64(previous, current));
            r[index + 1] = add64(mul64(r[index + 1], alpha), mul64(next, other_current));
            previous = next;
            index += 2;
        }
        const unsigned cross_end = (std::max)(2U, round_up(parameters.cross_order, 2));
        for (unsigned i = 0; i < cross_end; ++i) {
            const double next = other[i];
            r[index] = add64(mul64(r[index], alpha), mul64(previous, current));
            r[index + 1] = add64(mul64(r[index + 1], alpha), mul64(next, other_current));
            previous = next;
            index += 2;
        }
        r[index] = add64(mul64(r[index], alpha), mul64(previous, current));
    }

    void build_self(unsigned channel, double inverse_alpha, bool use_initial,
                    double weight, bool cross_boundary = true) {
        const double *r = correlations(channel);
        const double *self = samples(channel);
        const double *initial = initial_covariance[channel].data();
        double *rhs = coefficients(channel);
        const unsigned step = channels;
        for (unsigned i = 0; i < self_order; ++i) {
            double value = mul64(sub64(r[step * i], mul64(self[i], self[0])), inverse_alpha);
            if (use_initial) {
                value = add64(value, mul64(initial[i * kMatrixStride], weight));
            }
            matrix[i * kMatrixStride] = value;
            for (unsigned j = 1; j <= i; ++j) {
                value = mul64(sub64(matrix[(i - 1) * kMatrixStride + j - 1],
                    mul64(self[j], self[i])), inverse_alpha);
                if (use_initial) {
                    value = add64(value, mul64(initial[i * kMatrixStride + j], weight));
                }
                matrix[i * kMatrixStride + j] = value;
            }
            rhs[i] = r[step * (i + 1)];
            if (channels == 2 && cross_boundary) {
                if (self_order == kMatrixStride) {
                    // Escaped stereo (128,0): the native extra boundary row
                    // coincides with the beginning of channel-0 history. It
                    // is outside the solved matrix but is not an OOB write
                    // here; preserve its observable history side effect.
                    history[i] = r[2 * i + 1];
                } else {
                    matrix[self_order * kMatrixStride + i] = r[2 * i + 1];
                }
            }
        }
    }

    void build_cross(unsigned channel, double inverse_alpha, bool use_initial,
                     double weight) {
        const double *r = correlations(channel);
        const double *self = samples(channel);
        const double *other = samples(1 - channel);
        const double *initial = initial_covariance[channel].data();
        double *rhs = coefficients(channel);
        const unsigned n = parameters.order;
        for (unsigned i = self_order + 1; i < n; ++i) {
            const double lagged = other[i - self_order - 1];
            double value = mul64(sub64(r[2 * i], mul64(lagged, self[0])), inverse_alpha);
            if (use_initial) {
                value = add64(value, mul64(initial[i * kMatrixStride], weight));
            }
            matrix[i * kMatrixStride] = value;
            for (unsigned j = 1; j < self_order; ++j) {
                value = mul64(sub64(matrix[(i - 1) * kMatrixStride + j - 1],
                    mul64(lagged, self[j])), inverse_alpha);
                if (use_initial) {
                    value = add64(value, mul64(initial[i * kMatrixStride + j], weight));
                }
                matrix[i * kMatrixStride + j] = value;
            }
        }
        for (unsigned i = self_order; i < n; ++i) {
            matrix[i * kMatrixStride + self_order] = r[2 * i + 1];
            for (unsigned j = self_order + 1; j <= i; ++j) {
                double value = mul64(sub64(matrix[(i - 1) * kMatrixStride + j - 1],
                    mul64(other[j - self_order - 1], other[i - self_order - 1])), inverse_alpha);
                if (use_initial) {
                    value = add64(value, mul64(initial[i * kMatrixStride + j], weight));
                }
                matrix[i * kMatrixStride + j] = value;
            }
            rhs[i] = r[2 * (i + 1)];
        }
    }

    void identity(unsigned channel, unsigned length) {
        double *coefficient = coefficients(channel);
        coefficient[0] = 1.0;
        std::fill(coefficient + 1, coefficient + length, 0.0);
    }

    void recalculate(unsigned channel) {
        double *r = correlations(channel);
        const unsigned n = parameters.order;
        if (r[0] < 0.5) {
            identity(channel, n);
            return;
        }
        const bool use_initial = count[channel] < 48000;
        const double weight = use_initial
            ? initial_weight(alpha, count[channel] - self_order - 1) : 0.0;
        const double inverse_alpha = div64(1.0, alpha);
        build_self(channel, inverse_alpha, use_initial, weight);
        if (channels == 2) {
            build_cross(channel, inverse_alpha, use_initial, weight);
        }
        double *coefficient = coefficients(channel);
        if (solve(matrix.data(), coefficient, diagonal(), n)) {
            return;
        }
        std::fill(coefficient, coefficient + n, 0.0);
        for (unsigned i = 1; i <= n; ++i) {
            if (r[0] == r[channels * i]) {
                coefficient[i - 1] = 1.0;
                return;
            }
        }
        if (channels == 1) {
            coefficient[0] = 1.0;
            return;
        }
        // Stereo full-system failure retries just the self-history system.
        // Its initial covariance is added UNWEIGHTED, not with alpha^exponent.
        build_self(channel, inverse_alpha, use_initial, 1.0, false);
        if (!solve(matrix.data(), coefficient, diagonal(), self_order)) {
            identity(channel, self_order);
        }
    }

    void update(unsigned channel, double sample) {
        require_channel(channel);
        if (!std::isfinite(sample)) {
            throw EntropyError("non-finite reconstructed LPC sample");
        }
        insert(channel, sample);
        if (count[channel] < self_order + 1) {
            return;
        }
        if (count[channel] == self_order + 1) {
            seed(channel);
        }
        update_correlations(channel);
        if (count[channel] - last_recalculation[channel] < parameters.period) {
            return;
        }
        double *r = correlations(channel);
        if (r[0] < kZeroThreshold && r[0] != 0.0) {
            std::fill(r, r + channels * parameters.order + 1, 0.0);
        }
        if (!suppressed[channel]) {
            recalculate(channel);
            last_recalculation[channel] = count[channel];
        }
    }
};

LpcPredictor::LpcPredictor(const LpcParameters &parameters, unsigned channels)
try : impl_(std::make_unique<Impl>(parameters, channels)) {
} catch (const std::bad_alloc &) {
    throw UnsupportedCodec("insufficient memory for the LPC predictor");
}

LpcPredictor::~LpcPredictor() = default;
LpcPredictor::LpcPredictor(LpcPredictor &&) noexcept = default;
LpcPredictor &LpcPredictor::operator=(LpcPredictor &&) noexcept = default;

double LpcPredictor::predict(unsigned channel) {
    return impl_->predict(channel);
}

void LpcPredictor::update(unsigned channel, double sample) {
    impl_->update(channel, sample);
}

void LpcPredictor::suppress_recalculation(unsigned channel, bool suppress) {
    impl_->set_suppressed(channel, suppress);
}

void LpcPredictor::suppress_recalculation(bool suppress) {
    for (unsigned channel = 0; channel < impl_->channels; ++channel) {
        impl_->set_suppressed(channel, suppress);
    }
}

} // namespace predictor_detail
} // namespace openfrog
