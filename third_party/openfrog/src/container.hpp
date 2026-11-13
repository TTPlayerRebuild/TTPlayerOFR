#ifndef OPENFROG_CONTAINER_HPP
#define OPENFROG_CONTAINER_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace openfrog {

class Input {
public:
    virtual ~Input() = default;
    virtual int32_t read(void *buffer, uint32_t count) = 0;
    virtual bool seek(int64_t position) = 0;
    virtual int64_t tell() const = 0;
    virtual int64_t length() const = 0;
    virtual bool seekable() const = 0;
};

struct Tag {
    std::string key;
    std::string value;
};

struct Header {
    uint64_t total_samples = 0;
    uint8_t sample_type = 0;
    uint8_t channel_config = 0;
    uint32_t sample_rate = 0;
    uint16_t encoder_id = 0;
    uint8_t compression = 0;
    uint16_t minimum_version = 4500;

    uint32_t channels() const { return channel_config <= 1 ? channel_config + 1U : 0U; }
    uint32_t bits_per_sample() const {
        return sample_type <= 7 ? (sample_type / 2U + 1U) * 8U
                               : (sample_type <= 10 ? 32U : 0U);
    }
    uint32_t version() const { return (encoder_id >> 4) + 4500U; }
    uint64_t points() const { return channels() ? total_samples / channels() : 0; }
};

struct AudioBlock {
    uint32_t crc = 0;
    uint32_t sample_count = 0;
    uint8_t sample_type = 0;
    uint8_t channel_config = 0;
    uint16_t internal = 0;
    uint16_t encoder_id = 0;
    std::vector<uint8_t> coded;
    int64_t file_offset = -1;
    int64_t next_offset = -1;
    bool crc_valid = false;
};

// Resource limits of this implementation, NOT limits in the OptimFROG format.
// Oversized ordinary blocks (including unknown blocks) are rejected before
// allocation/skipping. Tag parsing is additionally bounded in bytes and items.
constexpr uint32_t kMaxContainerBlockBytes = 64U * 1024U * 1024U;
constexpr uint32_t kMaxContainerTagBytes = 16U * 1024U * 1024U;
constexpr uint32_t kMaxContainerApeItems = 65536U;
constexpr uint32_t kMaxContainerId3v2PrefixBytes = 1024U * 1024U;

// Standard reflected IEEE CRC-32: polynomial 0xedb88320, initial/final XOR -1.
uint32_t crc32(const uint8_t *data, std::size_t size);

// Parses the documented >= 15-byte main-header format. This is a container and
// tag reader, NOT an audio decoder. Input ownership/closing remains with the API.
// Strict policy: COMP blocks must have nonzero, channel-aligned sample counts,
// match the main sample format, and pass CRC32; no repair/silence substitution.
// EOF before TAIL is an error, not a successful end of audio.
class Container {
public:
    explicit Container(Input &input, bool strict_checks = true);
    void open(bool read_tags);
    bool next_audio_block(AudioBlock &block);
    // Does not skip COMP blocks. Call only after consuming all declared samples.
    void read_tail();
    bool rewind_audio();
    // Position at the independent COMP block containing an interleaved scalar
    // sample. Builds a compressed-block index without running a predictor.
    bool seek_audio(uint64_t scalar_position, uint64_t &block_start);

    const Header &header() const { return header_; }
    const std::vector<uint8_t> &head() const { return head_; }
    const std::vector<uint8_t> &tail() const { return tail_; }
    const std::vector<Tag> &tags() const { return tags_; }
    bool tail_recovery_attempted() const noexcept { return tail_recovery_attempted_; }

private:
    struct Chunk {
        uint32_t id = 0;
        uint32_t size = 0;
        int64_t offset = -1;
    };

    Input &input_;
    bool strict_checks_;
    struct IndexEntry {
        int64_t offset;
        int64_t next_offset;
        uint64_t sample_start;
        uint32_t sample_count;
    };
    std::vector<IndexEntry> index_;
    Header header_;
    std::vector<uint8_t> head_;
    std::vector<uint8_t> tail_;
    std::vector<Tag> tags_;
    int64_t position_ = 0;
    int64_t length_ = -1;
    int64_t first_audio_offset_ = -1;
    uint64_t samples_read_ = 0;
    Chunk pending_;
    bool have_pending_ = false;
    bool opened_ = false;
    bool failed_ = false;
    bool finished_ = false;
    bool tail_recovery_attempted_ = false;

    void require_open() const;
    void require_bytes(uint32_t count) const;
    void read_exact(void *buffer, uint32_t count);
    void seek_to(int64_t position);
    void skip(uint32_t count);
    void validate_chunk(const Chunk &chunk) const;
    Chunk read_chunk();
    Chunk take_chunk();
    void read_payload(std::vector<uint8_t> &data, uint32_t count);
    void consume_tail(const Chunk &chunk);
    void read_tags();
    void read_id3v1(const uint8_t *data);
    void read_apev2(const uint8_t *footer, int64_t tag_end);
    void add_tag(std::string key, std::string value);
};

} // namespace openfrog

#endif
