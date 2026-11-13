#ifndef OPENFROG_ENTROPY_HPP
#define OPENFROG_ENTROPY_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace openfrog {

// Independent residual/entropy primitives, NOT an OFR audio decoder. Sources:
// analysis/codec-analysis.md sections 4/5 and the retained 5.100 x86 pseudocode.
// No predictor, sample mapping, selector 3, CRC, or correction-file decoding.
class EntropyError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Returned by value: changing a snapshot cannot modify/restore decoder state.
struct RangeState {
    std::uint32_t code = 0;
    std::uint32_t range = 0x80;
    std::uint32_t unit = 0;
    std::uint8_t previous = 0;
    std::uint8_t tag = 0;
    std::size_t position = 0;
    bool finished = false;
    bool failed = false;
};

class AdaptiveTree;

class RangeDecoder {
public:
    static constexpr std::uint32_t normalization_threshold = 0x00800000U;

    // Owns the input vector. Reads tag + first code byte at position, exactly as
    // 10010a20 does. position is an absolute offset in this input vector.
    explicit RangeDecoder(std::vector<std::uint8_t> input, std::size_t position = 0);

    // AudioBlock.coded starts AFTER the complete 14-byte COMP payload header:
    // crc[4], samples[4], type[1], channels[1], internal[2], encoderID[2].
    // 1000a170 reads type/channels/internal, SKIPS encoderID's LOW byte, then
    // 10010a20 consumes encoderID's HIGH byte as tag and coded[0] as first byte.
    // Thus RangeDecoder(block.coded) would be OFF BY ONE. Use this factory:
    //   auto rc = RangeDecoder::from_comp_coded(block.coded, block.encoder_id);
    // No tag is prepended to the owned vector: snapshot.position indexes coded.
    // The range stream still contains mapping and predictor headers BEFORE the
    // entropy header. Do NOT initialize an EntropySelector directly here unless
    // those earlier headers have already been consumed by their real parsers.
    static RangeDecoder from_comp_coded(std::vector<std::uint8_t> coded,
        std::uint16_t encoder_id, std::size_t position = 0);

    RangeDecoder(const RangeDecoder &) = delete;
    RangeDecoder &operator=(const RangeDecoder &) = delete;
    RangeDecoder(RangeDecoder &&) noexcept = default;
    RangeDecoder &operator=(RangeDecoder &&) noexcept = default;

    RangeState state() const noexcept { return state_; }
    void normalize();
    std::uint32_t uniform(std::uint32_t count);
    std::uint32_t smallBits(unsigned count); // 0..12; zero still normalizes
    std::uint32_t bits(unsigned count);      // 0..32; zero consumes nothing
    std::uint32_t byteSymbol();
    std::uint32_t u16(); // two range-coded byte symbols, least significant first
    std::uint32_t u32(); // four symbols; NOT a raw little-endian input read

    // 10010a90: normalize, then detach/disable the reader. Does not validate CRC
    // or demand EOF. Repeated successful finish() is an idempotent API choice.
    // Reading after finish or a truncation/invalid-stream failure is rejected.
    void finish();

private:
    friend class AdaptiveTree;
    struct TagAlreadyRead {};
    RangeDecoder(std::vector<std::uint8_t> input, std::size_t position,
        std::uint8_t tag, TagAlreadyRead);
    std::vector<std::uint8_t> input_;
    RangeState state_;

    void require_active() const;
    [[noreturn]] void fail(const char *message);
    std::uint8_t read_byte();
    std::uint32_t scaled(std::uint32_t total);
    void consume(std::uint32_t cumulative, std::uint32_t frequency,
        std::uint32_t total);
};

std::int32_t unfold(std::uint32_t value) noexcept;

class AdaptiveTree {
public:
    // Implementation resource/budget bounds, NOT asserted format limits.
    static constexpr std::uint32_t max_alphabet = 65536;
    static constexpr std::uint32_t max_threshold =
        RangeDecoder::normalization_threshold - 2;

    AdaptiveTree(std::uint32_t alphabet, std::uint32_t threshold);
    std::uint32_t decode(RangeDecoder &range);

    std::uint32_t alphabet() const noexcept { return alphabet_; }
    std::uint32_t leaf_base() const noexcept { return leaf_base_; }
    std::uint32_t total() const noexcept { return total_; }
    std::uint32_t threshold() const noexcept { return threshold_; }
    std::uint32_t frequency(std::uint32_t symbol) const;

private:
    std::uint32_t alphabet_;
    std::uint32_t threshold_;
    std::uint32_t leaf_base_ = 1;
    std::uint32_t total_;
    std::vector<std::uint32_t> weights_;
    std::uint32_t rebuild(std::uint32_t node);
    void rescale();
};

struct EntropyState {
    unsigned bit_width = 0; // reduced-range signed width supplied by mapping
    unsigned channels = 0;  // actual 1 or 2, NOT the container's channel_config
    std::uint32_t decay = 0;
    double alpha = 0;
    double beta = 0;
    std::array<double, 2> estimates{{0, 0}}; // S (selector 1) / mean (selector 2)
    std::uint64_t decoded_samples = 0;      // interleaved scalar samples
    bool failed = false;
};

// Implementation allocation limit per decode() call, not a format/block limit.
// Longer blocks can be split into channel-aligned calls without resetting state.
constexpr std::size_t kMaxEntropyBatchSamples = 1024U * 1024U;

// Both constructors consume ONLY the entropy decay header at the range stream's
// CURRENT position (100045a0 / 10003c10). No guessed mapping/predictor defaults.
// IEEE binary64, round-to-nearest, no flush-to-zero/fast-math is required. Each
// floating operation is explicitly stored to binary64 to prevent FMA/extra
// precision; non-nearest/flushing modes are rejected without changing controls.
// RangeDecoder must outlive these objects and must not be moved while bound.
// decode() returns residuals, NOT predicted/reconstructed PCM. Stereo requests
// must contain whole interleaved frames. No implicit finish() or block framing.
class EntropySelector1 {
public:
    EntropySelector1(RangeDecoder &range, unsigned bit_width, unsigned channels);
    EntropySelector1(const EntropySelector1 &) = delete;
    EntropySelector1 &operator=(const EntropySelector1 &) = delete;
    std::vector<std::int32_t> decode(std::size_t sample_count);
    EntropyState state() const noexcept { return state_; }
    std::size_t context_count() const noexcept { return models_.size(); }
    const AdaptiveTree &model(std::size_t context) const;

private:
    RangeDecoder &range_;
    EntropyState state_;
    std::vector<AdaptiveTree> models_; // shared by both channels; independent S
    std::int32_t decode_one(unsigned channel);
};

class EntropySelector2 {
public:
    EntropySelector2(RangeDecoder &range, unsigned bit_width, unsigned channels);
    EntropySelector2(const EntropySelector2 &) = delete;
    EntropySelector2 &operator=(const EntropySelector2 &) = delete;
    std::vector<std::int32_t> decode(std::size_t sample_count);
    EntropyState state() const noexcept { return state_; }
    const AdaptiveTree &model() const noexcept { return model_; }

private:
    RangeDecoder &range_;
    EntropyState state_;
    AdaptiveTree model_; // one shared 32-symbol tree; independent channel means
    std::int32_t decode_one(unsigned channel);
};

} // namespace openfrog

#endif
