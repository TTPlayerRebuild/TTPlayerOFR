#ifndef OPENFROG_CODEC_HPP
#define OPENFROG_CODEC_HPP

#include "container.hpp"
#include "codec_types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

namespace openfrog {

// Stateful compressed-stream decoder. Counts are interleaved scalar samples;
// conversion to public points and packing belongs to the SDK boundary. For
// OFRX FLOAT32, returned int32_t words carry raw IEEE bits after DETA, not numeric
// integers; reconstruction errors are fatal rather than concealed as silence.
class AudioDecoder {
public:
    explicit AudioDecoder(Container &container);
    ~AudioDecoder();
    AudioDecoder(const AudioDecoder &) = delete;
    AudioDecoder &operator=(const AudioDecoder &) = delete;
    std::vector<std::int32_t> read(std::size_t scalar_count);
    bool seek(std::uint64_t scalar_position);
    std::uint64_t position() const noexcept;
    bool recoverable_errors() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openfrog
#endif
