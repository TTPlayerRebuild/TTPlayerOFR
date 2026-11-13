#include "container.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

namespace openfrog {
namespace {

// Layout: official SDK 1.600 Documentation/format.txt, OptimFROG 5.100.
// Tag behavior: retained REA pseudocode 0x10001320/1650/2340/3120/3360.
constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8)
        | (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(d) << 24);
}

constexpr uint32_t kMain = fourcc('O', 'F', 'R', ' ');
constexpr uint32_t kHead = fourcc('H', 'E', 'A', 'D');
constexpr uint32_t kComp = fourcc('C', 'O', 'M', 'P');
constexpr uint32_t kTail = fourcc('T', 'A', 'I', 'L');

uint16_t le16(const uint8_t *p) {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0])
        | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t le32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
        | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint32_t crc_update(uint32_t state, const uint8_t *data, std::size_t size) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> result{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t value = i;
            for (unsigned bit = 0; bit < 8; ++bit) {
                value = (value >> 1) ^ ((value & 1U) ? 0xedb88320U : 0U);
            }
            result[i] = value;
        }
        return result;
    }();
    for (std::size_t i = 0; i < size; ++i) {
        state = table[(state ^ data[i]) & 0xffU] ^ (state >> 8);
    }
    return state;
}

std::string first_string(const uint8_t *data, std::size_t size) {
    if (!size) {
        return {};
    }
    const uint8_t *end = std::find(data, data + size, uint8_t(0));
    return std::string(reinterpret_cast<const char *>(data),
        static_cast<std::size_t>(end - data));
}

std::string strip_spaces(std::string value) {
    const std::size_t first = value.find_first_not_of(' ');
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = value.find_last_not_of(' ');
    return value.substr(first, last - first + 1);
}

unsigned char ascii_fold(unsigned char value) {
    return value >= 'A' && value <= 'Z'
        ? static_cast<unsigned char>(value + ('a' - 'A')) : value;
}

bool same_key(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_fold(static_cast<unsigned char>(a[i]))
            != ascii_fold(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// The conventional 148 ID3v1/Winamp genre labels; these are format data.
const char *const kId3Genres[] = {
    "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge",
    "Hip-Hop", "Jazz", "Metal", "New Age", "Oldies", "Other", "Pop", "R&B",
    "Rap", "Reggae", "Rock", "Techno", "Industrial", "Alternative", "Ska",
    "Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient",
    "Trip-Hop", "Vocal", "Jazz+Funk", "Fusion", "Trance", "Classical",
    "Instrumental", "Acid", "House", "Game", "Sound Clip", "Gospel",
    "Noise", "AlternRock", "Bass", "Soul", "Punk", "Space", "Meditative",
    "Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic", "Darkwave",
    "Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream",
    "Southern Rock", "Comedy", "Cult", "Gangsta", "Top 40", "Christian Rap",
    "Pop/Funk", "Jungle", "Native American", "Cabaret", "New Wave",
    "Psychadelic", "Rave", "Showtunes", "Trailer", "Lo-Fi", "Tribal",
    "Acid Punk", "Acid Jazz", "Polka", "Retro", "Musical", "Rock & Roll",
    "Hard Rock", "Folk", "Folk-Rock", "National Folk", "Swing", "Fast Fusion",
    "Bebob", "Latin", "Revival", "Celtic", "Bluegrass", "Avantgarde",
    "Gothic Rock", "Progressive Rock", "Psychedelic Rock", "Symphonic Rock",
    "Slow Rock", "Big Band", "Chorus", "Easy Listening", "Acoustic", "Humour",
    "Speech", "Chanson", "Opera", "Chamber Music", "Sonata", "Symphony",
    "Booty Bass", "Primus", "Porn Groove", "Satire", "Slow Jam", "Club",
    "Tango", "Samba", "Folklore", "Ballad", "Power Ballad", "Rhythmic Soul",
    "Freestyle", "Duet", "Punk Rock", "Drum Solo", "A capella", "Euro-House",
    "Dance Hall", "Goa", "Drum & Bass", "Club-House", "Hardcore", "Terror",
    "Indie", "BritPop", "Negerpunk", "Polsk Punk", "Beat", "Christian Gangsta",
    "Heavy Metal", "Black Metal", "Crossover", "Contemporary Christian",
    "Christian Rock", "Merengue", "Salsa", "Thrash Metal", "Anime", "JPop",
    "SynthPop"
};
static_assert(sizeof(kId3Genres) / sizeof(kId3Genres[0]) == 148,
    "ID3v1 genre table must contain 148 entries");

} // namespace

uint32_t crc32(const uint8_t *data, std::size_t size) {
    if (!data && size) {
        throw std::runtime_error("CRC input is null");
    }
    return crc_update(0xffffffffU, data, size) ^ 0xffffffffU;
}

Container::Container(Input &input, bool strict_checks)
    : input_(input), strict_checks_(strict_checks) {}

void Container::require_open() const {
    if (!opened_ || failed_) {
        throw std::runtime_error("container is not open or is in an error state");
    }
}

void Container::require_bytes(uint32_t count) const {
    if (position_ < 0 || position_ > (std::numeric_limits<int64_t>::max)() - count) {
        throw std::runtime_error("container offset overflow");
    }
    if (length_ >= 0 && (position_ > length_ || count > length_ - position_)) {
        throw std::runtime_error("truncated container");
    }
}

void Container::read_exact(void *buffer, uint32_t count) {
    require_bytes(count);
    uint8_t *destination = static_cast<uint8_t *>(buffer);
    while (count) {
        const uint32_t requested = (std::min)(count, uint32_t(65536));
        const int32_t received = input_.read(destination, requested);
        if (received < 0) {
            throw std::runtime_error("input read failed");
        }
        if (!received) {
            throw std::runtime_error("truncated container");
        }
        if (static_cast<uint32_t>(received) > requested) {
            throw std::runtime_error("input read returned more bytes than requested");
        }
        destination += received;
        count -= static_cast<uint32_t>(received);
        position_ += received;
    }
}

void Container::seek_to(int64_t position) {
    if (position < 0 || (length_ >= 0 && position > length_)
        || !input_.seek(position)) {
        throw std::runtime_error("input seek failed");
    }
    const int64_t reported = input_.tell();
    if (reported >= 0 && reported != position) {
        throw std::runtime_error("input seek returned an incorrect position");
    }
    position_ = position;
}

void Container::skip(uint32_t count) {
    require_bytes(count);
    if (!count) {
        return;
    }
    if (input_.seekable() && length_ >= 0) {
        seek_to(position_ + count);
        return;
    }
    std::array<uint8_t, 4096> scratch{};
    while (count) {
        const uint32_t part = (std::min)(count, static_cast<uint32_t>(scratch.size()));
        read_exact(scratch.data(), part);
        count -= part;
    }
}

void Container::validate_chunk(const Chunk &chunk) const {
    if (chunk.size > kMaxContainerBlockBytes) {
        throw std::runtime_error("container block exceeds the 64 MiB implementation limit");
    }
    // The native open does not require the first COMP payload to be present.
    // Its range reader discovers truncation during decoding, permitting the
    // declared audio to follow the observed damaged-block recovery policy.
    if (strict_checks_ || chunk.id != kComp) { require_bytes(chunk.size); }
    if (chunk.id == kComp && chunk.size < 14) {
        throw std::runtime_error("COMP payload is shorter than its 14-byte header");
    }
}

Container::Chunk Container::read_chunk() {
    Chunk chunk;
    chunk.offset = position_;
    uint8_t data[8];
    read_exact(data, sizeof(data));
    chunk.id = le32(data);
    chunk.size = le32(data + 4);
    validate_chunk(chunk);
    return chunk;
}

Container::Chunk Container::take_chunk() {
    if (have_pending_) {
        have_pending_ = false;
        return pending_;
    }
    return read_chunk();
}

void Container::read_payload(std::vector<uint8_t> &data, uint32_t count) {
    require_bytes(count);
    data.resize(count);
    read_exact(data.data(), count);
}

void Container::open(bool want_tags) {
    header_ = Header{};
    head_.clear();
    tail_.clear();
    tags_.clear();
    first_audio_offset_ = -1;
    index_.clear();
    samples_read_ = 0;
    have_pending_ = false;
    opened_ = false;
    failed_ = false;
    finished_ = false;
    tail_recovery_attempted_ = false;
    position_ = input_.tell();
    if (position_ < 0) {
        // An unpositioned sequential input is assumed to start at byte zero.
        position_ = 0;
    }
    length_ = input_.length();

    try {
        uint32_t prefix_bytes = 0;
        Chunk main;
        if (!strict_checks_) {
            // 1000ad10 scans a rolling signature up to 1 MiB. A self-extracting
            // prefix/ID3v2 is not itself parsed or trusted as executable code.
            std::array<uint8_t, 8> window{};
            read_exact(window.data(), 8);
            if (std::memcmp(window.data(), "*RIF", 4) == 0) {
                throw std::runtime_error("legacy *RIFF is rejected by this DLL");
            }
            for (;;) {
                const uint32_t size = le32(window.data() + 4);
                if (le32(window.data()) == kMain && size > 11 && size < 256) {
                    main = Chunk{kMain, size, position_ - 8};
                    break;
                }
                if (prefix_bytes == kMaxContainerId3v2PrefixBytes) {
                    throw std::runtime_error("no OFR signature within the 1 MiB search window");
                }
                std::move(window.begin() + 1, window.end(), window.begin());
                read_exact(window.data() + 7, 1);
                ++prefix_bytes;
            }
        } else for (;;) {
            uint8_t data[10];
            main.offset = position_;
            read_exact(data, 8);
            if (std::memcmp(data, "ID3", 3) != 0) {
                main.id = le32(data);
                main.size = le32(data + 4);
                break;
            }
            read_exact(data + 8, 2);
            if (data[3] < 2 || data[3] > 4 || data[4] == 0xff
                || ((data[6] | data[7] | data[8] | data[9]) & 0x80)) {
                throw std::runtime_error("invalid ID3v2 prefix header");
            }
            const uint8_t flag_mask = data[3] == 2 ? 0xc0 : (data[3] == 3 ? 0xe0 : 0xf0);
            if (data[5] & static_cast<uint8_t>(~flag_mask)) {
                throw std::runtime_error("invalid ID3v2 prefix flags");
            }
            const uint32_t size = (static_cast<uint32_t>(data[6]) << 21)
                | (static_cast<uint32_t>(data[7]) << 14)
                | (static_cast<uint32_t>(data[8]) << 7) | data[9];
            const uint32_t footer = data[3] == 4 && (data[5] & 0x10) ? 10U : 0U;
            const uint32_t total = 10U + size + footer;
            if (total > kMaxContainerId3v2PrefixBytes - prefix_bytes) {
                throw std::runtime_error("ID3v2 prefix exceeds the 1 MiB implementation limit");
            }
            prefix_bytes += total;
            skip(size + footer);
        }
        if (main.id != kMain) {
            throw std::runtime_error("not an OFR container (legacy *RIFF/OFRX is unsupported)");
        }
        validate_chunk(main);
        if (main.size < 15 || (!strict_checks_ && main.size > 79)) {
            throw std::runtime_error("invalid or unsupported OFR main-header length");
        }
        uint8_t data[17];
        const uint32_t fixed = main.size >= 17 ? 17U : 15U;
        read_exact(data, fixed);
        header_.total_samples = le32(data) | (static_cast<uint64_t>(le16(data + 4)) << 32);
        header_.sample_type = data[6];
        header_.channel_config = data[7];
        header_.sample_rate = le32(data + 8);
        header_.encoder_id = le16(data + 12);
        header_.compression = data[14];
        const uint32_t minimum_version = main.size >= 17 ? 4500U + le16(data + 15) : 4500U;
        if (minimum_version > 5100) {
            throw std::runtime_error("file requires a decoder newer than version 5.100");
        }
        header_.minimum_version = static_cast<uint16_t>(minimum_version);
        if (!header_.bits_per_sample() || !header_.channels() || !header_.sample_rate
            || header_.total_samples % header_.channels()
            || (header_.compression >> 3) > 14 || (header_.compression & 7) > 2) {
            throw std::runtime_error("invalid OFR sample format, rate, count, or compression");
        }
        skip(main.size - fixed);

        bool have_head = false;
        for (;;) {
            const Chunk chunk = read_chunk();
            if (chunk.id == kHead) {
                if (have_head) {
                    throw std::runtime_error("duplicate HEAD block");
                }
                read_payload(head_, chunk.size);
                have_head = true;
                if (!strict_checks_) {
                    // Native open stops after HEAD. Missing/truncated COMP is
                    // detected at read(), not reclassified as an open failure.
                    first_audio_offset_ = position_;
                    break;
                }
            } else if (chunk.id == kComp || chunk.id == kTail) {
                if (!have_head) {
                    throw std::runtime_error("missing HEAD block before audio");
                }
                if (chunk.id == kTail && header_.total_samples) {
                    throw std::runtime_error("missing COMP data before TAIL");
                }
                first_audio_offset_ = chunk.offset;
                pending_ = chunk;
                have_pending_ = true;
                break;
            } else if (chunk.id == kMain) {
                throw std::runtime_error("duplicate OFR main block");
            } else {
                if (!strict_checks_) { throw std::runtime_error("expected HEAD after OFR main header"); }
                skip(chunk.size);
            }
        }
        if (want_tags) {
            read_tags();
        }
        opened_ = true;
    } catch (...) {
        failed_ = true;
        throw;
    }
}

void Container::consume_tail(const Chunk &chunk) {
    if (samples_read_ != header_.total_samples) {
        throw std::runtime_error("TAIL precedes the declared number of samples");
    }
    read_payload(tail_, chunk.size);
    finished_ = true;
}

bool Container::next_audio_block(AudioBlock &block) {
    require_open();
    if (finished_) {
        return false;
    }
    try {
        if (samples_read_ == header_.total_samples) {
            read_tail();
            return false;
        }
        for (;;) {
            const Chunk chunk = take_chunk();
            if (chunk.id == kTail) {
                consume_tail(chunk);
                return false;
            }
            if (chunk.id == kComp) {
                uint8_t data[14];
                read_exact(data, sizeof(data));
                AudioBlock result;
                result.crc = le32(data);
                result.sample_count = le32(data + 4);
                result.sample_type = data[8];
                result.channel_config = data[9];
                result.internal = le16(data + 10);
                result.encoder_id = le16(data + 12);
                result.file_offset = chunk.offset;
                if (!result.sample_count || result.sample_count % header_.channels()
                    || result.sample_count > header_.total_samples - samples_read_
                    || result.sample_type > 10 || result.channel_config > 1
                    || result.sample_count % (result.channel_config + 1U)
                    || (strict_checks_ && (result.sample_type != header_.sample_type
                        || result.channel_config != header_.channel_config))) {
                    throw std::runtime_error("inconsistent COMP sample count or format");
                }
                if (strict_checks_) {
                    read_payload(result.coded, chunk.size - 14);
                } else {
                    result.coded.resize(chunk.size - 14);
                    std::size_t received = 0;
                    while (received < result.coded.size()) {
                        const auto remaining = static_cast<uint32_t>(result.coded.size() - received);
                        const int32_t count = input_.read(result.coded.data() + received, remaining);
                        if (count < 0 || static_cast<uint32_t>(count) > remaining) {
                            throw std::runtime_error("input read failed in COMP payload");
                        }
                        if (!count) { break; }
                        received += static_cast<uint32_t>(count);
                        position_ += count;
                    }
                    result.coded.resize(received); // Never pad a truncated code stream with zeroes.
                }
                uint32_t checksum = crc_update(0xffffffffU, data + 4, 10);
                checksum = crc_update(checksum, result.coded.data(), result.coded.size());
                result.crc_valid = (checksum ^ 0xffffffffU) == result.crc;
                if (strict_checks_ && !result.crc_valid) {
                    throw std::runtime_error("COMP CRC32 mismatch");
                }
                result.next_offset = position_;
                if (index_.empty() || index_.back().offset < result.file_offset) {
                    index_.push_back(IndexEntry{result.file_offset, result.next_offset,
                        samples_read_, result.sample_count});
                }
                samples_read_ += result.sample_count;
                block = std::move(result);
                return true;
            }
            if (chunk.id == kHead || chunk.id == kMain) {
                throw std::runtime_error("unexpected header block in audio data");
            }
            skip(chunk.size);
        }
    } catch (...) {
        failed_ = true;
        throw;
    }
}

void Container::read_tail() {
    require_open();
    if (finished_) {
        return;
    }
    if (samples_read_ != header_.total_samples) {
        // A premature request must not consume or discard any audio.
        throw std::runtime_error("TAIL requested before all COMP samples were consumed");
    }
    try {
        if (!strict_checks_) {
            Chunk chunk;
            if (have_pending_) {
                chunk = take_chunk();
            } else {
                chunk.offset = position_;
                uint8_t id[4];
                try { read_exact(id, 4); }
                catch (...) { tail_recovery_attempted_ = true; throw; }
                chunk.id = le32(id);
                if (chunk.id != kTail) {
                    tail_recovery_attempted_ = true;
                    throw std::runtime_error("expected TAIL after the audio blocks");
                }
                uint8_t size[4];
                read_exact(size, 4);
                chunk.size = le32(size);
                validate_chunk(chunk);
            }
            consume_tail(chunk);
            return;
        }
        for (;;) {
            const Chunk chunk = take_chunk();
            if (chunk.id == kTail) {
                consume_tail(chunk);
                return;
            }
            if (chunk.id == kComp) {
                throw std::runtime_error("COMP data exceeds the declared sample count");
            }
            if (chunk.id == kHead || chunk.id == kMain) {
                throw std::runtime_error("unexpected header block before TAIL");
            }
            skip(chunk.size);
        }
    } catch (...) {
        failed_ = true;
        throw;
    }
}

bool Container::rewind_audio() {
    if (!opened_ || first_audio_offset_ < 0 || !input_.seekable()) {
        return false;
    }
    try {
        seek_to(first_audio_offset_);
    } catch (const std::runtime_error &) {
        failed_ = true;
        return false;
    }
    samples_read_ = 0;
    have_pending_ = false;
    tail_.clear();
    finished_ = false;
    failed_ = false;
    return true;
}

bool Container::seek_audio(uint64_t target, uint64_t &block_start) {
    if (!opened_ || !input_.seekable() || first_audio_offset_ < 0
        || target >= header_.total_samples) {
        return false;
    }
    auto select = [&](const IndexEntry &entry) {
        seek_to(entry.offset);
        samples_read_ = entry.sample_start;
        have_pending_ = false;
        finished_ = false;
        failed_ = false;
        tail_.clear();
        block_start = entry.sample_start;
    };
    try {
        const auto found = std::upper_bound(index_.begin(), index_.end(), target,
            [](uint64_t sample, const IndexEntry &entry) { return sample < entry.sample_start; });
        if (found != index_.begin()) {
            const auto &entry = *(found - 1);
            if (target - entry.sample_start < entry.sample_count) {
                select(entry);
                return true;
            }
        }
        uint64_t start = index_.empty() ? 0
            : index_.back().sample_start + index_.back().sample_count;
        seek_to(index_.empty() ? first_audio_offset_ : index_.back().next_offset);
        have_pending_ = false;
        for (;;) {
            const Chunk chunk = read_chunk();
            if (chunk.id == kTail) { throw std::runtime_error("TAIL precedes seek target"); }
            if (chunk.id != kComp) {
                if (chunk.id == kMain || chunk.id == kHead) {
                    throw std::runtime_error("unexpected header while indexing audio");
                }
                skip(chunk.size);
                continue;
            }
            uint8_t data[14];
            read_exact(data, sizeof(data));
            const uint32_t count = le32(data + 4);
            if (!count || count > header_.total_samples - start
                || count % header_.channels() || data[8] > 10 || data[9] > 1
                || count % (data[9] + 1U)
                || (strict_checks_ && (data[8] != header_.sample_type
                    || data[9] != header_.channel_config))) {
                throw std::runtime_error("invalid COMP header while indexing audio");
            }
            const int64_t next = position_ + (chunk.size - 14U);
            IndexEntry entry{chunk.offset, next, start, count};
            index_.push_back(entry);
            if (target - start < count) {
                select(entry);
                return true;
            }
            skip(chunk.size - 14U);
            start += count;
        }
    } catch (...) {
        failed_ = true;
        throw;
    }
}

void Container::add_tag(std::string key, std::string value) {
    value = strip_spaces(std::move(value));
    if (key.empty() || value.empty()) {
        return;
    }
    for (Tag &tag : tags_) {
        if (same_key(tag.key, key)) {
            tag.value = std::move(value); // Preserve the first spelling and order.
            return;
        }
    }
    if (tags_.size() < 64) {
        tags_.push_back(Tag{std::move(key), std::move(value)});
    }
}

void Container::read_id3v1(const uint8_t *data) {
    add_tag("Title", first_string(data + 3, 30));
    add_tag("Artist", first_string(data + 33, 30));
    add_tag("Album", first_string(data + 63, 30));
    add_tag("Year", first_string(data + 93, 4));
    // REA: byte 125 alone selects the 28-byte v1.1 comment, even for track zero.
    add_tag("Comment", first_string(data + 97, data[125] ? 30 : 28));
    if (!data[125] && data[126]) {
        add_tag("Track", std::to_string(static_cast<unsigned>(data[126])));
    }
    if (data[127] != 255) {
        add_tag("Genre", data[127] < 148 ? kId3Genres[data[127]] : "Invalid");
    }
}

void Container::read_apev2(const uint8_t *footer, int64_t tag_end) {
    const uint32_t version = le32(footer + 8);
    const uint32_t size = le32(footer + 12);
    const uint32_t count = le32(footer + 16);
    const uint32_t flags = le32(footer + 20);
    if (version != 2000 || (flags & 0x60000000U)) {
        return; // Not a supported APEv2 footer, as in REA 0x10001650.
    }
    if (size < 32 || size > kMaxContainerTagBytes || size > tag_end) {
        throw std::runtime_error("invalid APEv2 size or 16 MiB tag limit exceeded");
    }
    if (count > kMaxContainerApeItems || count > (size - 32) / 9) {
        throw std::runtime_error("invalid APEv2 item count or item limit exceeded");
    }
    seek_to(tag_end - size);
    std::vector<uint8_t> items;
    read_payload(items, size - 32); // Tag size includes the footer, not its optional header.
    std::size_t position = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (items.size() - position < 8) {
            throw std::runtime_error("truncated APEv2 item header");
        }
        const uint32_t value_size = le32(items.data() + position);
        const uint32_t item_flags = le32(items.data() + position + 4);
        position += 8;
        const auto begin = items.begin() + static_cast<std::ptrdiff_t>(position);
        const auto end = std::find(begin, items.end(), uint8_t(0));
        if (end == items.end()) {
            throw std::runtime_error("unterminated APEv2 key");
        }
        const std::string key(begin, end);
        position = static_cast<std::size_t>(end - items.begin()) + 1;
        if (value_size > items.size() - position) {
            throw std::runtime_error("truncated APEv2 value");
        }
        if ((item_flags & 6U) != 2U) {
            add_tag(key, first_string(items.data() + position, value_size));
        }
        position += value_size;
    }
}

void Container::read_tags() {
    // End tags cannot be fetched from a sequential or unknown-length input.
    if (!input_.seekable() || length_ < 0) {
        return;
    }
    const int64_t saved_position = position_;
    try {
        int64_t tag_end = length_;
        if (tag_end >= 128) {
            std::array<uint8_t, 128> id3{};
            seek_to(tag_end - 128);
            read_exact(id3.data(), static_cast<uint32_t>(id3.size()));
            if (std::memcmp(id3.data(), "TAG", 3) == 0) {
                read_id3v1(id3.data());
                tag_end -= 128;
            }
        }
        if (tag_end >= 32) {
            std::array<uint8_t, 32> footer{};
            seek_to(tag_end - 32);
            read_exact(footer.data(), static_cast<uint32_t>(footer.size()));
            if (std::memcmp(footer.data(), "APETAGEX", 8) == 0) {
                read_apev2(footer.data(), tag_end);
            }
        }
    } catch (...) {
        const std::exception_ptr error = std::current_exception();
        seek_to(saved_position);
        std::rethrow_exception(error);
    }
    // position_ is AFTER the pending chunk's header. Do not seek to its start
    // here: doing so would interpret its header bytes as compressed payload.
    seek_to(saved_position);
}

} // namespace openfrog
