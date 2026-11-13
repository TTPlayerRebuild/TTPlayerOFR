#include "entropy.hpp"

#include <algorithm>
#include <cfenv>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#if defined(__FAST_MATH__) || (defined(_M_FP_FAST) && _M_FP_FAST)
#error OptimFROG entropy models require strict IEEE floating-point arithmetic.
#endif

namespace openfrog {
namespace {

static_assert(sizeof(double) == sizeof(std::uint64_t)
    && std::numeric_limits<double>::is_iec559
    && std::numeric_limits<double>::digits == 53
    && std::numeric_limits<double>::max_exponent == 1024,
    "Entropy models require IEEE-754 binary64 double");

// Volatile stores deliberately separate each operation. In particular beta is
// 1-alpha, not 1/N, and the square/decay additions must not become FMAs. This
// also fixes x87 excess precision when compiled for 32-bit hosts.
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
        throw EntropyError("entropy requires round-to-nearest floating arithmetic");
    }
    // fegetround does not expose FTZ/DAZ. Probe gradual binary64 underflow
    // without platform-specific intrinsics or changing the caller's controls.
    volatile double tiny = std::numeric_limits<double>::denorm_min();
    volatile double doubled = tiny + tiny;
    const double stored = doubled;
    std::uint64_t representation = 0;
    std::memcpy(&representation, &stored, sizeof(representation));
    if (representation != 2) {
        throw EntropyError("entropy requires gradual binary64 underflow (no FTZ/DAZ)");
    }
}

unsigned floor_log2(std::uint32_t value) {
    if (!value) {
        throw EntropyError("log2 of zero in entropy model");
    }
    unsigned result = 0;
    while (value > 1) {
        value >>= 1;
        ++result;
    }
    return result;
}

std::uint32_t power_of_two(unsigned exponent) {
    if (exponent >= 32) {
        throw EntropyError("entropy shift exceeds uint32 width");
    }
    return std::uint32_t(1) << exponent;
}

// Explicit modulo-2^32 arithmetic matches the unsigned x86 operations in
// 10003c90/10003e50; it is not C++ signed overflow or an unchecked narrowing.
std::uint32_t low32(std::uint64_t value) noexcept {
    return static_cast<std::uint32_t>(value & UINT64_C(0xffffffff));
}

unsigned energy_context(double energy, std::size_t count) {
    if (!std::isfinite(energy) || energy < 1.0) {
        throw EntropyError("invalid squared-mean entropy state");
    }
    std::uint64_t representation = 0;
    std::memcpy(&representation, &energy, sizeof(representation));
    const auto exponent = static_cast<unsigned>((representation >> 52) & 0x7ffU);
    if (exponent < 1023 || exponent == 0x7ffU) {
        throw EntropyError("invalid squared-mean exponent");
    }
    const unsigned context = exponent - 1023;
    if (context >= count) {
        throw EntropyError("squared-mean context exceeds model array");
    }
    return context;
}

EntropyState read_entropy_header(RangeDecoder &range, unsigned width,
    unsigned channels, double initial) {
    if (width < 1 || width > 32 || channels < 1 || channels > 2) {
        throw EntropyError("entropy requires width 1..32 and one or two channels");
    }
    require_nearest();
    const auto selector = range.smallBits(12); // 100045a0 / 10003c10
    const auto decay = selector == 0xfffU ? range.u16() + 0x1001U : selector + 2U;
    if (decay < 2 || decay > 69632U) {
        throw EntropyError("invalid entropy decay parameter");
    }
    EntropyState result;
    result.bit_width = width;
    result.channels = channels;
    result.decay = decay;
    result.alpha = div64(sub64(static_cast<double>(decay), 1.0),
        static_cast<double>(decay));
    result.beta = sub64(1.0, result.alpha);
    if (!(result.alpha > 0.0 && result.alpha < 1.0 && result.beta > 0.0)) {
        throw EntropyError("invalid entropy decay coefficients");
    }
    result.estimates.fill(initial);
    return result;
}

void require_decode(const RangeDecoder &range, const EntropyState &state,
    std::size_t sample_count) {
    const auto rc = range.state();
    if (state.failed || rc.failed || rc.finished) {
        throw EntropyError("entropy decoder is closed or failed");
    }
    if (sample_count > kMaxEntropyBatchSamples || sample_count % state.channels
        || sample_count > (std::numeric_limits<std::uint64_t>::max)()
            - state.decoded_samples) {
        throw EntropyError("invalid entropy batch size or partial stereo frame");
    }
    require_nearest();
}

} // namespace

RangeDecoder::RangeDecoder(std::vector<std::uint8_t> input, std::size_t position)
    : input_(std::move(input)) {
    if (position > input_.size()) {
        throw EntropyError("range start exceeds input size");
    }
    state_.position = position;
    state_.tag = read_byte();             // 10010a20: tag, then first byte
    state_.previous = read_byte();
    state_.code = state_.previous >> 1;
}

RangeDecoder::RangeDecoder(std::vector<std::uint8_t> input, std::size_t position,
    std::uint8_t tag, TagAlreadyRead) : input_(std::move(input)) {
    if (position > input_.size()) {
        throw EntropyError("coded range start exceeds input size");
    }
    state_.position = position;
    state_.tag = tag;
    state_.previous = read_byte();
    state_.code = state_.previous >> 1;
}

RangeDecoder RangeDecoder::from_comp_coded(std::vector<std::uint8_t> coded,
    std::uint16_t encoder_id, std::size_t position) {
    return RangeDecoder(std::move(coded), position,
        static_cast<std::uint8_t>(encoder_id >> 8), TagAlreadyRead{});
}

void RangeDecoder::require_active() const {
    if (state_.finished || state_.failed) {
        throw EntropyError("range decoder is closed or failed");
    }
}

[[noreturn]] void RangeDecoder::fail(const char *message) {
    state_.failed = true;
    throw EntropyError(message);
}

std::uint8_t RangeDecoder::read_byte() {
    require_active();
    if (state_.position >= input_.size()) {
        fail("truncated range stream");
    }
    return input_[state_.position++];
}

void RangeDecoder::normalize() {
    require_active();
    if (!state_.range || state_.code >= state_.range) {
        fail("invalid range invariant");
    }
    while (state_.range <= normalization_threshold) { // inclusive, 10003960
        const auto next = read_byte();
        const auto next_range = static_cast<std::uint64_t>(state_.range) << 8;
        const auto next_code = (static_cast<std::uint64_t>(state_.code) << 8)
            | ((static_cast<std::uint32_t>(state_.previous) & 1U) << 7)
            | (static_cast<std::uint32_t>(next) >> 1);
        if (next_range > (std::numeric_limits<std::uint32_t>::max)()
            || next_code >= next_range) {
            fail("range normalization overflow");
        }
        state_.range = static_cast<std::uint32_t>(next_range);
        state_.code = static_cast<std::uint32_t>(next_code);
        state_.previous = next;
    }
}

std::uint32_t RangeDecoder::scaled(std::uint32_t total) {
    if (!total) {
        throw EntropyError("zero range alphabet");
    }
    normalize();
    const auto unit = state_.range / total;
    if (!unit) {
        fail("range alphabet exceeds normalized interval");
    }
    state_.unit = unit;
    return (std::min)(state_.code / unit, total - 1U);
}

void RangeDecoder::consume(std::uint32_t cumulative, std::uint32_t frequency,
    std::uint32_t total) {
    const auto end = static_cast<std::uint64_t>(cumulative) + frequency;
    if (!frequency || !total || end > total || !state_.unit
        || state_.unit != state_.range / total) {
        fail("invalid range frequency interval");
    }
    const auto delta = static_cast<std::uint64_t>(state_.unit) * cumulative;
    if (delta > state_.code) {
        fail("range cumulative frequency exceeds code");
    }
    const auto next_range = end < total
        ? static_cast<std::uint64_t>(state_.unit) * frequency
        : static_cast<std::uint64_t>(state_.range) - delta;
    if (!next_range
        || next_range > (std::numeric_limits<std::uint32_t>::max)()
        || state_.code - delta >= next_range) {
        fail("range frequency arithmetic overflow");
    }
    state_.code -= static_cast<std::uint32_t>(delta);
    state_.range = static_cast<std::uint32_t>(next_range);
}

std::uint32_t RangeDecoder::uniform(std::uint32_t count) {
    require_active();
    const auto symbol = scaled(count);
    consume(symbol, 1, count);
    return symbol;
}

std::uint32_t RangeDecoder::smallBits(unsigned count) {
    require_active();
    if (count > 12) {
        throw EntropyError("smallBits supports only 0..12 bits");
    }
    return uniform(power_of_two(count));
}

std::uint32_t RangeDecoder::bits(unsigned count) {
    require_active();
    if (count > 32) {
        throw EntropyError("bits supports only 0..32 bits");
    }
    std::uint32_t result = 0;
    unsigned shift = 0;
    while (count > 12) {
        result |= smallBits(12) << shift; // shift is 0 or 12 here
        shift += 12;
        count -= 12;
    }
    if (count) {
        // For the last group shift <= 24 and shift + count <= 32.
        if (shift >= 32 || count > 32 - shift) {
            fail("segmented integer shift overflow");
        }
        result |= smallBits(count) << shift;
    }
    return result;
}

std::uint32_t RangeDecoder::byteSymbol() {
    return uniform(256);
}

std::uint32_t RangeDecoder::u16() {
    const auto low = byteSymbol();
    return low | (byteSymbol() << 8);
}

std::uint32_t RangeDecoder::u32() {
    std::uint32_t result = byteSymbol();
    result |= byteSymbol() << 8;
    result |= byteSymbol() << 16;
    result |= byteSymbol() << 24;
    return result;
}

void RangeDecoder::finish() {
    if (state_.finished && !state_.failed) {
        return;
    }
    normalize();
    state_.finished = true; // equivalent to detaching 10010a90's input reader
}

std::int32_t unfold(std::uint32_t value) noexcept {
    const std::int64_t magnitude = value >> 1;
    return static_cast<std::int32_t>((value & 1U) ? -magnitude - 1 : magnitude);
}

AdaptiveTree::AdaptiveTree(std::uint32_t alphabet, std::uint32_t threshold)
    : alphabet_(alphabet), threshold_(threshold), total_(alphabet) {
    if (alphabet < 2 || alphabet > max_alphabet || threshold < alphabet
        || threshold > max_threshold) {
        throw EntropyError("invalid adaptive alphabet or threshold");
    }
    while (leaf_base_ < alphabet_) {
        leaf_base_ <<= 1; // <= 65536, checked before allocation/any tree shifts
    }
    weights_.assign(static_cast<std::size_t>(leaf_base_) * 2, 0);
    for (std::uint32_t i = 0; i < alphabet_; ++i) {
        weights_[leaf_base_ + i] = 1;
    }
    if (rebuild(1) != total_) {
        throw EntropyError("adaptive tree initialization mismatch");
    }
}

std::uint32_t AdaptiveTree::frequency(std::uint32_t symbol) const {
    if (symbol >= alphabet_) {
        throw EntropyError("adaptive symbol index out of bounds");
    }
    return weights_.at(leaf_base_ + symbol);
}

std::uint32_t AdaptiveTree::rebuild(std::uint32_t node) {
    if (node >= weights_.size()) {
        throw EntropyError("adaptive node index out of bounds");
    }
    if (node >= leaf_base_) {
        return weights_[node];
    }
    const auto left = rebuild(node * 2);
    const auto right = rebuild(node * 2 + 1);
    if (right > (std::numeric_limits<std::uint32_t>::max)() - left) {
        throw EntropyError("adaptive subtree frequency overflow");
    }
    weights_[node] = left; // left subtotal, not the total of both children
    return left + right;
}

void AdaptiveTree::rescale() {
    std::uint64_t sum = 0;
    for (std::uint32_t i = 0; i < alphabet_; ++i) {
        auto &frequency = weights_.at(leaf_base_ + i);
        if (!frequency) {
            throw EntropyError("zero adaptive leaf frequency");
        }
        frequency = ((frequency - 1U) >> 1) + 1U; // ceil(f/2), 10002c50
        sum += frequency;
    }
    if (!sum || sum > max_threshold + 2U) {
        throw EntropyError("rescaled adaptive total exceeds budget");
    }
    total_ = static_cast<std::uint32_t>(sum);
    if (rebuild(1) != total_) {
        throw EntropyError("rescaled adaptive tree mismatch");
    }
}

std::uint32_t AdaptiveTree::decode(RangeDecoder &range) {
    if (!total_ || total_ > max_threshold + 2U) {
        throw EntropyError("invalid adaptive total");
    }
    const auto slot = range.scaled(total_);
    std::uint32_t node = 1;
    std::uint32_t cumulative = 0;
    while (node < leaf_base_) {
        const auto left = weights_.at(node);
        if (left > total_ - cumulative) {
            throw EntropyError("invalid adaptive left frequency");
        }
        if (slot < cumulative + left) {
            node *= 2;
        } else {
            cumulative += left;
            node = node * 2 + 1;
        }
    }
    if (node - leaf_base_ >= alphabet_) {
        throw EntropyError("decoded a padded adaptive symbol");
    }
    const auto frequency = weights_.at(node);
    if (!frequency || frequency > total_ - cumulative
        || total_ > (std::numeric_limits<std::uint32_t>::max)() - 2U) {
        throw EntropyError("invalid adaptive leaf frequency");
    }
    range.consume(cumulative, frequency, total_);
    weights_[node] += 2; // bounded by total_ + 2, which was checked above
    // Updating ancestors after a successful range consumption is equivalent on
    // success to 10003b40's updates while descending, but avoids partial trees.
    for (auto child = node; child > 1; child >>= 1) {
        if (!(child & 1U)) {
            weights_.at(child >> 1) += 2;
        }
    }
    total_ += 2;
    if (total_ >= threshold_) {
        rescale();
    }
    return node - leaf_base_;
}

EntropySelector1::EntropySelector1(RangeDecoder &range, unsigned bit_width,
    unsigned channels)
    : range_(range), state_(read_entropy_header(range, bit_width, channels, 1.0)) {
    // Hidden 10009f30 arguments verified from 10004634..1000465d bytecode:
    // contexts=2*B, alphabet=(B<=3 ? 1<<B : 8*B-16), threshold=0x8000.
    const auto alphabet = bit_width <= 3 ? power_of_two(bit_width)
                                        : bit_width * 8U - 16U;
    models_.reserve(static_cast<std::size_t>(bit_width) * 2);
    for (unsigned i = 0; i < bit_width * 2U; ++i) {
        models_.emplace_back(alphabet, 0x8000);
    }
}

const AdaptiveTree &EntropySelector1::model(std::size_t context) const {
    if (context >= models_.size()) {
        throw EntropyError("entropy context index out of bounds");
    }
    return models_[context];
}

std::int32_t EntropySelector1::decode_one(unsigned channel) {
    double &energy = state_.estimates.at(channel);
    const auto context = energy_context(energy, models_.size());
    const auto symbol = models_.at(context).decode(range_);
    std::uint32_t value = symbol;
    if (symbol >= 8) {
        const auto exponent = (symbol - 8U) >> 3;
        const auto mantissa = (symbol - 8U) & 7U;
        if (exponent > 28) {
            throw EntropyError("squared-mean integer shift out of bounds");
        }
        const auto wide = (static_cast<std::uint64_t>(8U + mantissa) << exponent)
            + range_.bits(exponent);
        if (wide > (std::numeric_limits<std::uint32_t>::max)()) {
            throw EntropyError("squared-mean integer overflow");
        }
        value = static_cast<std::uint32_t>(wide);
    }
    const auto limit = state_.bit_width == 32
        ? (std::numeric_limits<std::uint32_t>::max)()
        : power_of_two(state_.bit_width) - 1U;
    if (value > limit) {
        throw EntropyError("squared-mean residual exceeds configured width");
    }
    const double raw = static_cast<double>(value);
    const double decayed = mul64(state_.alpha, energy);
    double contribution = mul64(raw, raw);
    contribution = mul64(contribution, state_.beta);
    contribution = add64(contribution, state_.beta);
    const double next = add64(contribution, decayed);
    energy_context(next, models_.size()); // reject nonfinite/out-of-bounds state
    energy = next;
    return unfold(value);
}

std::vector<std::int32_t> EntropySelector1::decode(std::size_t sample_count) {
    require_decode(range_, state_, sample_count);
    std::vector<std::int32_t> output(sample_count);
    try {
        for (std::size_t i = 0; i < sample_count; ++i) {
            output[i] = decode_one(static_cast<unsigned>(i % state_.channels));
            ++state_.decoded_samples;
        }
    } catch (...) {
        state_.failed = true;
        throw;
    }
    return output;
}

EntropySelector2::EntropySelector2(RangeDecoder &range, unsigned bit_width,
    unsigned channels)
    : range_(range), state_(read_entropy_header(range, bit_width, channels, 0.0)),
      model_(32, 0x8000) {}

std::int32_t EntropySelector2::decode_one(unsigned channel) {
    double &mean = state_.estimates.at(channel);
    if (!std::isfinite(mean) || mean < 0.0 || mean >= 4294967296.0) {
        throw EntropyError("mean cannot be converted to uint32");
    }
    // 10003eb0 and the corresponding helpers set x87 RC=0x0c00 for FISTP:
    // truncate toward zero, NOT round-to-nearest despite Ghidra's ROUND().
    const auto integral = static_cast<std::uint32_t>(mean);
    const auto quotient = (integral >> 2) + 1U;
    const bool short_tail = quotient <= 0x4000U;
    const auto exponent = short_tail ? 0U : floor_log2(quotient);
    const auto step = short_tail ? quotient : power_of_two(exponent);
    const auto group = model_.decode(range_);
    std::uint32_t value = 0;
    if (group != 31) {
        const auto tail = short_tail ? range_.uniform(quotient)
                                     : range_.bits(exponent);
        value = low32(static_cast<std::uint64_t>(group) * step + tail);
    } else {
        const auto escape_base = floor_log2(low32(static_cast<std::uint64_t>(step) * 31U));
        if (escape_base >= state_.bit_width) {
            throw EntropyError("entropy escape exceeds configured width");
        }
        const auto tail_bits = escape_base
            + range_.uniform(state_.bit_width - escape_base);
        // bit_width <= 32 and escape range excludes the endpoint: <= 31.
        value = power_of_two(tail_bits) + range_.bits(tail_bits);
    }
    const double decayed = mul64(state_.alpha, mean);
    const double contribution = mul64(static_cast<double>(value), state_.beta);
    const double next = add64(decayed, contribution);
    if (!std::isfinite(next) || next < 0.0 || next >= 4294967296.0) {
        throw EntropyError("updated entropy mean exceeds uint32 domain");
    }
    mean = next;
    return unfold(value);
}

std::vector<std::int32_t> EntropySelector2::decode(std::size_t sample_count) {
    require_decode(range_, state_, sample_count);
    std::vector<std::int32_t> output(sample_count);
    try {
        for (std::size_t i = 0; i < sample_count; ++i) {
            output[i] = decode_one(static_cast<unsigned>(i % state_.channels));
            ++state_.decoded_samples;
        }
    } catch (...) {
        state_.failed = true;
        throw;
    }
    return output;
}

} // namespace openfrog
