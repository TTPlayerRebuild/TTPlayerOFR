#include "codec.hpp"
#include "entropy.hpp"
#include "entropy3.hpp"
#include "float_details.hpp"
#include "mapping.hpp"
#include "predictor.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace openfrog {
namespace {

unsigned sample_bits(std::uint8_t type) {
    if (type > 10) { throw UnsupportedCodec("unsupported COMP sample format"); }
    return type < 8 ? (type / 2U + 1U) * 8U : 32U;
}

RangeDecoder block_range(AudioBlock &block) {
    if (block.sample_type >= 8 && !block.has_float_details) {
        throw UnsupportedCodec("FLOAT32 decoding requires an OFRX COMP/DETA pair");
    }
    const unsigned mapping = block.internal & 0x3fU;
    const unsigned predictor = (block.internal >> 6) & 0x1fU;
    const unsigned entropy = block.internal >> 11;
    if ((mapping != 1 && mapping != 2) || predictor < 1 || predictor > 4
        || entropy < 1 || entropy > 3 || block.channel_config > 1 || !block.sample_count) {
        throw UnsupportedCodec("unknown COMP algorithm selector");
    }
    return RangeDecoder::from_comp_coded(std::move(block.coded), block.encoder_id);
}

class BlockDecoder {
public:
    explicit BlockDecoder(AudioBlock block)
        : channels_(block.channel_config + 1U), total_(block.sample_count),
          range_(block_range(block)) {
        const unsigned mapping = block.internal & 0x3fU;
        const unsigned predictor = (block.internal >> 6) & 0x1fU;
        const unsigned entropy = block.internal >> 11;
        if ((mapping != 1 && mapping != 2) || predictor < 1 || predictor > 4
            || entropy < 1 || entropy > 3 || channels_ > 2 || !total_) {
            throw UnsupportedCodec("unknown COMP algorithm selector");
        }
        mapping_ = std::make_unique<SampleMapping>(mapping, sample_bits(block.sample_type),
            channels_, predictor == 4, range_);
        const auto &spec = mapping_->spec();
        predictor_ = std::make_unique<Predictor>(predictor, spec, total_, range_);
        switch (entropy) {
        case 1: entropy1_ = std::make_unique<EntropySelector1>(range_, spec.bit_width, channels_); break;
        case 2: entropy2_ = std::make_unique<EntropySelector2>(range_, spec.bit_width, channels_); break;
        case 3: entropy3_ = std::make_unique<EntropySelector3>(range_, spec.bit_width, channels_, total_); break;
        default: throw UnsupportedCodec("unknown entropy selector");
        }
        if (block.has_float_details) {
            details_ = std::make_unique<FloatDetails>(std::move(block.details_coded), total_);
        }
    }

    std::vector<std::int32_t> read(std::size_t count) {
        if (count > total_ - consumed_ || count % channels_) {
            throw std::runtime_error("invalid scalar batch for COMP decoder");
        }
        std::vector<std::int32_t> samples;
        samples.reserve(count);
        while (samples.size() < count) {
            const auto part = (std::min)(count - samples.size(), kMaxEntropyBatchSamples);
            std::vector<std::int32_t> batch;
            if (entropy1_) { batch = entropy1_->decode(part); }
            else if (entropy2_) { batch = entropy2_->decode(part); }
            else { batch = entropy3_->decode(part); }
            predictor_->reconstruct(batch.data(), part);
            mapping_->apply(batch.data(), part);
            if (details_) { details_->reconstruct(batch.data(), part); }
            samples.insert(samples.end(), batch.begin(), batch.end());
            consumed_ += static_cast<std::uint32_t>(part);
        }
        if (consumed_ == total_) { range_.finish(); }
        return samples;
    }
private:
    unsigned channels_;
    std::uint32_t total_, consumed_ = 0;
    RangeDecoder range_;
    std::unique_ptr<SampleMapping> mapping_;
    std::unique_ptr<Predictor> predictor_;
    std::unique_ptr<EntropySelector1> entropy1_;
    std::unique_ptr<EntropySelector2> entropy2_;
    std::unique_ptr<EntropySelector3> entropy3_;
    std::unique_ptr<FloatDetails> details_;
};

} // namespace

class AudioDecoder::Impl {
public:
    explicit Impl(Container &value) : container(value) {}
    Container &container;
    std::unique_ptr<BlockDecoder> block;
    std::uint64_t position = 0;
    std::uint32_t block_remaining = 0;
    unsigned block_channels = 0;
    bool recovered = false;

    void next_block() {
        AudioBlock encoded;
        if (!container.next_audio_block(encoded)) {
            throw std::runtime_error("audio ended before the declared sample count");
        }
        block_remaining = encoded.sample_count;
        block_channels = encoded.channel_config + 1U;
        try {
            block = std::make_unique<BlockDecoder>(std::move(encoded));
        } catch (const UnsupportedCodec &) { throw; }
        catch (const std::bad_alloc &) { throw; }
        catch (const std::exception &) {
            // The OFF file decoder aborts on a broken DETA stream. Never turn
            // an unimplemented/invalid float reconstruction into apparent +0.
            if (container.header().floating_point()) { throw; }
            // 1000badf: the declared samples of a damaged block are concealed.
            block.reset();
            recovered = true;
        }
    }

    std::vector<std::int32_t> read(std::size_t count) {
        const auto available = container.header().total_samples - position;
        count = static_cast<std::size_t>((std::min)(std::uint64_t(count), available));
        std::vector<std::int32_t> result;
        result.reserve(count);
        while (result.size() < count) {
            if (!block_remaining) { next_block(); }
            const auto part = (std::min)(count - result.size(), std::size_t(block_remaining));
            if (part % block_channels) {
                throw std::runtime_error("partial COMP channel frame");
            }
            const auto old_size = result.size();
            if (block) {
                try {
                    auto batch = block->read(part);
                    result.insert(result.end(), batch.begin(), batch.end());
                } catch (const UnsupportedCodec &) { throw; }
                catch (const std::bad_alloc &) { throw; }
                catch (const std::exception &) {
                    if (container.header().floating_point()) { throw; }
                    block.reset();
                    recovered = true;
                    result.resize(old_size + part, 0);
                }
            } else { result.resize(old_size + part, 0); }
            block_remaining -= static_cast<std::uint32_t>(part);
            position += part;
            if (!block_remaining) { block.reset(); }
        }
        return result;
    }
};

AudioDecoder::AudioDecoder(Container &container) : impl_(std::make_unique<Impl>(container)) {}
AudioDecoder::~AudioDecoder() = default;
std::vector<std::int32_t> AudioDecoder::read(std::size_t count) { return impl_->read(count); }
std::uint64_t AudioDecoder::position() const noexcept { return impl_->position; }
bool AudioDecoder::recoverable_errors() const noexcept { return impl_->recovered; }

bool AudioDecoder::seek(std::uint64_t target) {
    if (target >= impl_->container.header().total_samples) { return false; }
    std::uint64_t start = 0;
    if (!impl_->container.seek_audio(target, start)) { return false; }
    impl_->block.reset();
    impl_->block_remaining = 0;
    impl_->position = start;
    while (impl_->position < target) {
        const auto remaining = target - impl_->position;
        const auto count = static_cast<std::size_t>((std::min)(remaining, std::uint64_t(22050)));
        impl_->read(count);
    }
    return impl_->position == target;
}

} // namespace openfrog
