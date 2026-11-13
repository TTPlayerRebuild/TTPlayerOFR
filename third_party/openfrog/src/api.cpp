#include "OptimFROG.h"
#include "container.hpp"
#include "codec.hpp"
#include "pcm.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cfenv>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <sys/stat.h>
#endif
#if defined(_M_IX86)
#include <float.h>
#endif
#if defined(_M_IX86) || defined(_M_X64) || defined(__SSE2__)
#include <xmmintrin.h>
#endif

namespace {

class FloatingMode {
public:
    FloatingMode() {
        have_environment_ = std::feholdexcept(&environment_) == 0;
#if defined(_M_IX86)
        unsigned control = 0;
        _controlfp_s(&control, _PC_53, _MCW_PC);
#endif
#if defined(_M_IX86) || defined(_M_X64) || defined(__SSE2__)
        mxcsr_ = _mm_getcsr();
        _mm_setcsr(0x1f80);
#endif
        std::fesetround(FE_TONEAREST);
    }
    ~FloatingMode() {
        if (have_environment_) { std::fesetenv(&environment_); }
#if defined(_M_IX86) || defined(_M_X64) || defined(__SSE2__)
        _mm_setcsr(mxcsr_);
#endif
    }
private:
    std::fenv_t environment_{};
    bool have_environment_ = false;
#if defined(_M_IX86) || defined(_M_X64) || defined(__SSE2__)
    unsigned mxcsr_ = 0;
#endif
};

class Source : public openfrog::Input {
public:
    virtual bool close() = 0;
};

class FileSource final : public Source {
public:
    explicit FileSource(const char *name) {
        if (!name || !(file_ = std::fopen(name, "rb"))) {
            throw std::runtime_error("cannot open input file");
        }
        const int64_t start = tell();
#ifdef _WIN32
        if (start >= 0 && _fseeki64(file_, 0, SEEK_END) == 0) {
#else
        if (start >= 0 && fseeko(file_, 0, SEEK_END) == 0) {
#endif
            length_ = tell();
            if (!seek(start)) {
                close();
                throw std::runtime_error("cannot restore input file position");
            }
            seekable_ = length_ >= 0;
        }
    }
    ~FileSource() override { close(); }
    int32_t read(void *destination, uint32_t count) override {
        if (!file_ || count > static_cast<uint32_t>(INT32_MAX)) { return -1; }
        const std::size_t received = std::fread(destination, 1, count, file_);
        return received ? static_cast<int32_t>(received) : (std::ferror(file_) ? -1 : 0);
    }
    bool seek(int64_t position) override {
        if (!file_ || position < 0) { return false; }
#ifdef _WIN32
        return _fseeki64(file_, position, SEEK_SET) == 0;
#else
        return fseeko(file_, static_cast<off_t>(position), SEEK_SET) == 0;
#endif
    }
    int64_t tell() const override {
        if (!file_) { return -1; }
#ifdef _WIN32
        return _ftelli64(file_);
#else
        return static_cast<int64_t>(ftello(file_));
#endif
    }
    int64_t length() const override { return length_; }
    bool seekable() const override { return seekable_; }
    bool same_file(const char *name) const {
        // XP has file IDs; std::filesystem::equivalent also pulls in newer
        // precompiled STL filesystem code and its modern static OS imports.
        if (!file_) { return false; }
#ifdef _WIN32
        const HANDLE destination = CreateFileA(name, 0,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (destination == INVALID_HANDLE_VALUE) { return false; }
        BY_HANDLE_FILE_INFORMATION source_info{}, destination_info{};
        const HANDLE source = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file_)));
        const bool valid = GetFileInformationByHandle(source, &source_info)
            && GetFileInformationByHandle(destination, &destination_info);
        CloseHandle(destination);
        return valid && source_info.dwVolumeSerialNumber == destination_info.dwVolumeSerialNumber
            && source_info.nFileIndexHigh == destination_info.nFileIndexHigh
            && source_info.nFileIndexLow == destination_info.nFileIndexLow;
#else
        struct stat source_info{}, destination_info{};
        return fstat(fileno(file_), &source_info) == 0 && stat(name, &destination_info) == 0
            && source_info.st_dev == destination_info.st_dev && source_info.st_ino == destination_info.st_ino;
#endif
    }
    bool close() override {
        FILE *file = file_;
        file_ = nullptr;
        return !file || std::fclose(file) == 0;
    }
private:
    FILE *file_ = nullptr;
    int64_t length_ = -1;
    bool seekable_ = false;
};

class CallbackSource final : public Source {
public:
    CallbackSource(const ReadInterface &reader, void *instance)
        : reader_(reader), instance_(instance) {}
    ~CallbackSource() override { try { close(); } catch (...) {} }
    int32_t read(void *destination, uint32_t count) override {
        return closed_ ? -1 : reader_.read(instance_, destination, count);
    }
    bool seek(int64_t position) override {
        return !closed_ && reader_.seek && reader_.seek(instance_, position) != C_FALSE;
    }
    int64_t tell() const override {
        return !closed_ && reader_.getPos ? reader_.getPos(instance_) : -1;
    }
    int64_t length() const override {
        return !closed_ && reader_.length ? reader_.length(instance_) : -1;
    }
    bool seekable() const override {
        return !closed_ && reader_.seek && reader_.getPos &&
            reader_.seekable(instance_) != C_FALSE;
    }
    bool close() override {
        if (closed_) { return true; }
        closed_ = true; // Never invoke a client close callback twice, even on throw.
        return reader_.close(instance_) != C_FALSE;
    }
private:
    ReadInterface reader_;
    void *instance_;
    bool closed_ = false;
};

const char *const methods[] = {"fast", "normal", "high", "extra", "best", "ultra",
    "insane", "highnew", "extranew", "bestnew", "ultranew", "extrafast",
    "turbonew", "fastnew", "normalnew"};
const char *const speeds[] = {"1x", "2x", "4x"};
// FLOAT32_1 is the string observed in this particular DLL, not a correction
// of the official format document's FLOAT32_0 enum spelling.
const char *const sample_types[] = {"UINT8", "SINT8", "UINT16", "SINT16", "UINT24",
    "SINT24", "UINT32", "SINT32", "FLOAT32_1", "FLOAT32_16", "FLOAT32_24"};
const char *const channel_configs[] = {"MONO", "STEREO_LR"};

struct Decoder {
    std::unique_ptr<Source> input;
    std::unique_ptr<openfrog::Container> container;
    std::unique_ptr<openfrog::AudioDecoder> audio;
    OptimFROG_Info info{};
    int64_t position = 0;
    bool failed = false;
    bool tail_error = false;

    bool close() {
        audio.reset();
        container.reset();
        std::unique_ptr<Source> old = std::move(input);
        info = {};
        position = 0;
        failed = false;
        tail_error = false;
        return !old || old->close();
    }

    void open(std::unique_ptr<Source> source, bool tags) {
        close();
        input = std::move(source);
        try {
            // 1000b880 skips the stored COMP CRC. The standalone container
            // parser remains strict by default; the compatible SDK uses the
            // same no-CRC-rejection policy as this analyzed DLL.
            container = std::make_unique<openfrog::Container>(*input, false);
            container->open(tags);
            const auto &h = container->header();
            info.channels = h.channels();
            info.samplerate = h.sample_rate;
            info.bitspersample = h.bits_per_sample();
            info.version = h.version();
            info.method = methods[h.compression >> 3];
            info.speedup = speeds[h.compression & 7];
            info.noPoints = static_cast<int64_t>(h.points());
            // REA assembly 0x10002590..25b3 includes HEAD but not deferred TAIL.
            info.originalSize = static_cast<int64_t>(container->head().size()
                + h.total_samples * (info.bitspersample / 8));
            const int64_t size = input->seekable() ? input->length() : -1;
            info.compressedSize = size > 0 ? size : 0;
            info.length_ms = static_cast<int64_t>(h.points() * 1000U / h.sample_rate);
            // The original multiplies in 32 bits before treating this as unsigned.
            const uint32_t raw_rate = info.bitspersample * info.channels * info.samplerate;
            const long double rate = info.originalSize && info.compressedSize
                ? (static_cast<long double>(info.compressedSize) / info.originalSize)
                    * raw_rate / 1000.0L
                : static_cast<long double>(raw_rate) / 1000.0L;
            if (rate < 0 || rate > (std::numeric_limits<uint32_t>::max)()) {
                throw std::runtime_error("bitrate outside the ABI integer range");
            }
            info.bitrate = static_cast<uint32_t>(rate);
            info.sampleType = sample_types[h.sample_type];
            info.channelConfig = channel_configs[h.channel_config];
            audio = std::make_unique<openfrog::AudioDecoder>(*container);
        } catch (...) {
            close();
            throw;
        }
    }
};

Decoder *decoder(void *instance) { return static_cast<Decoder *>(instance); }

int32_t copy_bytes(const std::vector<uint8_t> &bytes, void *destination, uint32_t capacity) {
    if (bytes.size() > static_cast<std::size_t>(INT32_MAX)) { return -1; }
    if (bytes.size() <= capacity && !bytes.empty()) {
        if (!destination) { return -1; }
        std::memcpy(destination, bytes.data(), bytes.size());
    }
    return static_cast<int32_t>(bytes.size());
}

char *copy_string(const std::string &value) {
    void *memory = std::malloc(value.size() + 1);
    if (!memory) { throw std::bad_alloc(); }
    std::memcpy(memory, value.c_str(), value.size() + 1);
    return static_cast<char *>(memory);
}

bool write_bytes(FILE *file, const std::vector<uint8_t> &data) {
    return data.empty() || std::fwrite(data.data(), 1, data.size(), file) == data.size();
}

} // namespace

extern "C" {

uint32_t OFR_CALL OptimFROG_getVersion(void) { return 5100; }

void *OFR_CALL OptimFROG_createInstance(void) {
    try { return new Decoder; } catch (...) { return nullptr; }
}

void OFR_CALL OptimFROG_destroyInstance(void *instance) {
    if (auto *d = decoder(instance)) {
        try { d->close(); } catch (...) {}
        delete d;
    }
}

condition_t OFR_CALL OptimFROG_close(void *instance) {
    try { return instance && decoder(instance)->close() ? C_TRUE : C_FALSE; }
    catch (...) { return C_FALSE; }
}

condition_t OFR_CALL OptimFROG_open(void *instance, char *name, condition_t tags) {
    if (!instance || !name) { return C_FALSE; }
    try {
        decoder(instance)->open(std::make_unique<FileSource>(name), tags != C_FALSE);
        return C_TRUE;
    } catch (...) { return C_FALSE; }
}

condition_t OFR_CALL OptimFROG_openExt(void *instance, ReadInterface *reader,
    void *reader_instance, condition_t tags) {
    if (!instance || !reader || !reader->read || !reader->close || !reader->seekable) {
        return C_FALSE;
    }
    bool adapted = false;
    try {
        auto source = std::make_unique<CallbackSource>(*reader, reader_instance);
        adapted = true;
        decoder(instance)->open(std::move(source), tags != C_FALSE);
        return C_TRUE;
    } catch (...) {
        if (!adapted) {
            try { reader->close(reader_instance); } catch (...) {}
        }
        return C_FALSE;
    }
}

condition_t OFR_CALL OptimFROG_getInfo(void *instance, OptimFROG_Info *info) {
    if (!instance || !info || !decoder(instance)->container) { return C_FALSE; }
    *info = decoder(instance)->info;
    return C_TRUE;
}

condition_t OFR_CALL OptimFROG_getTags(void *instance, OptimFROG_Tags *tags) {
    if (!tags) { return C_FALSE; }
    *tags = {};
    if (!instance || !decoder(instance)->container) { return C_FALSE; }
    try {
        const auto &values = decoder(instance)->container->tags();
        if (values.empty()) { return C_FALSE; } // REA 0x10001e90.
        for (const auto &tag : values) {
            const uint32_t index = tags->keyCount++;
            tags->keys[index] = copy_string(tag.key);
            tags->values[index] = copy_string(tag.value);
        }
        return C_TRUE;
    } catch (...) {
        OptimFROG_freeTags(tags);
        return C_FALSE;
    }
}

void OFR_CALL OptimFROG_freeTags(OptimFROG_Tags *tags) {
    if (!tags) { return; }
    const uint32_t count = (std::min)(tags->keyCount, uint32_t(64));
    for (uint32_t i = 0; i < count; ++i) {
        std::free(tags->keys[i]);
        std::free(tags->values[i]);
        tags->keys[i] = nullptr;
        tags->values[i] = nullptr;
    }
    tags->keyCount = 0;
}

int32_t OFR_CALL OptimFROG_readHead(void *instance, void *data, uint32_t capacity) {
    if (!instance || !decoder(instance)->container) { return -1; }
    return copy_bytes(decoder(instance)->container->head(), data, capacity);
}

int32_t OFR_CALL OptimFROG_readTail(void *instance, void *data, uint32_t capacity) {
    auto *d = decoder(instance);
    if (!d || !d->container || d->failed || d->position != d->info.noPoints) { return -1; }
    try {
        // A failed seek changes only the public position to EOF. The native
        // tail probe then encounters COMP instead of TAIL and flags recovery.
        if (d->audio->position() != d->container->header().total_samples) { d->tail_error = true; }
        d->container->read_tail();
        return copy_bytes(d->container->tail(), data, capacity);
    } catch (...) {
        d->tail_error = d->tail_error || d->container->tail_recovery_attempted();
        return -1;
    }
}

int64_t OFR_CALL OptimFROG_getPos(void *instance) {
    return instance ? decoder(instance)->position : -1;
}

condition_t OFR_CALL OptimFROG_seekable(void *instance) {
    try {
        const auto *d = decoder(instance);
        return d && d->container && d->input->seekable() ? C_TRUE : C_FALSE;
    } catch (...) { return C_FALSE; }
}

condition_t OFR_CALL OptimFROG_seekPoint(void *instance, int64_t point) {
    auto *d = decoder(instance);
    if (!d || !d->container) { return C_FALSE; }
    try {
        if (point >= 0 && point < d->info.noPoints && OptimFROG_seekable(instance)) {
            if (!d->failed && point == d->position) { return C_TRUE; }
            FloatingMode floating_mode;
            if (d->audio->seek(static_cast<uint64_t>(point) * d->info.channels)) {
                d->position = point;
                d->failed = false;
                return C_TRUE;
            }
        }
    } catch (...) {}
    d->position = d->info.noPoints; // The observed public failure state is EOF.
    d->failed = false;
    return C_FALSE;
}

condition_t OFR_CALL OptimFROG_seekTime(void *instance, int64_t milliseconds) {
    const auto *d = decoder(instance);
    if (!d || !d->container) { return C_FALSE; }
    FloatingMode floating_mode;
    // 100021f0: FILD int64, FDIV 1000, FMUL rate, then truncate to int64.
    // Divide the exact integer before binary64 rounding (not an initial lossy
    // int64->double cast). Only the last ~2.5% of int64/1000 exceeds 2^53;
    // there, ULP=2 and an odd quotient with a remainder is above the midpoint.
    const uint64_t magnitude = milliseconds < 0 ? uint64_t(0) - static_cast<uint64_t>(milliseconds)
        : static_cast<uint64_t>(milliseconds);
    const uint64_t whole = magnitude / 1000;
    const uint64_t remainder = magnitude % 1000;
    double seconds;
    if (whole > UINT64_C(9007199254740992)) {
        seconds = static_cast<double>(whole + ((whole & 1U) && remainder ? 1U : 0U));
    } else {
        volatile double fraction = static_cast<double>(remainder) / 1000.0;
        volatile double sum = static_cast<double>(whole) + fraction;
        seconds = sum;
    }
    if (milliseconds < 0) { seconds = -seconds; }
    volatile double points = seconds * static_cast<double>(d->info.samplerate);
    if (!(points >= -9223372036854775808.0 && points < 9223372036854775808.0)) {
        return OptimFROG_seekPoint(instance, INT64_MIN);
    }
    return OptimFROG_seekPoint(instance, static_cast<int64_t>(points));
}

condition_t OFR_CALL OptimFROG_recoverableErrors(void *instance) {
    const auto *d = decoder(instance);
    return d && d->audio && (d->tail_error || d->audio->recoverable_errors()) ? C_TRUE : C_FALSE;
}

int32_t OFR_CALL OptimFROG_read(void *instance, void *data, uint32_t points, condition_t max16bit) {
    auto *d = decoder(instance);
    if (!d || !d->container || d->failed) { return -1; }
    if (!points || d->position == d->info.noPoints) { return 0; }
    if (!data) { return -1; }
    // New OFRX/FLOAT32 support is an extension beyond the analyzed SDK. There
    // is no native max16bit float contract; reject without consuming/poisoning
    // state rather than truncate IEEE words or silently change buffer widths.
    if (max16bit != C_FALSE && d->container->header().floating_point()) { return -1; }
    try {
        FloatingMode floating_mode;
        const uint64_t actual_points = (std::min)(uint64_t(points),
            static_cast<uint64_t>(d->info.noPoints - d->position));
        const uint64_t scalars = actual_points * d->info.channels;
        if (actual_points > INT32_MAX || scalars > (std::numeric_limits<std::size_t>::max)() / 4) {
            return -1;
        }
        const auto type = d->container->header().sample_type;
        const auto bytes = openfrog::packed_pcm_size(static_cast<std::size_t>(scalars),
            type, max16bit != C_FALSE);
        // Concealment is scoped to the requested part of each COMP block, as in
        // 1000bb54. Do not create smaller public recovery boundaries here.
        auto samples = d->audio->read(static_cast<std::size_t>(scalars));
        if (samples.size() != scalars) { throw std::runtime_error("short internal audio decode"); }
        openfrog::pack_pcm(samples.data(), samples.size(), type, max16bit != C_FALSE,
            static_cast<uint8_t *>(data), bytes);
        d->position += static_cast<int64_t>(actual_points);
        return static_cast<int32_t>(actual_points);
    } catch (...) {
        d->failed = true;
        return -1;
    }
}

int32_t OFR_CALL OptimFROG_infoFile(char *source, OptimFROG_Info *info, OptimFROG_Tags *tags) {
    if (!source || !info) { return OptimFROG_OpenError; }
    if (tags) { *tags = {}; }
    void *d = OptimFROG_createInstance();
    if (!d) { return OptimFROG_MemoryError; }
    if (!OptimFROG_open(d, source, tags ? C_TRUE : C_FALSE)) {
        OptimFROG_destroyInstance(d);
        return OptimFROG_OpenError;
    }
    OptimFROG_getInfo(d, info);
    if (tags) { OptimFROG_getTags(d, tags); }
    OptimFROG_destroyInstance(d);
    return OptimFROG_NoError;
}

int32_t OFR_CALL OptimFROG_decodeFile(char *source, char *destination,
    OptimFROG_callBack callback, void *callback_parameter) {
    if (!source || !destination) { return OptimFROG_OpenError; }
    try {
        Decoder d;
        FileSource *source_file = nullptr;
        try {
            auto input = std::make_unique<FileSource>(source);
            source_file = input.get();
            d.open(std::move(input), false);
        }
        catch (const std::bad_alloc &) { return OptimFROG_MemoryError; }
        catch (...) { return OptimFROG_OpenError; }
        if (source_file->same_file(destination)) {
            return OptimFROG_WriteError;
        }
        using File = std::unique_ptr<FILE, int (*)(FILE *)>;
        File out(std::fopen(destination, "wb"), &std::fclose);
        if (!out) { return OptimFROG_WriteError; }
        if (!write_bytes(out.get(), d.container->head())) { return OptimFROG_WriteError; }
        std::vector<uint8_t> buffer(176400);
        const uint32_t bytes_per_point = d.info.channels * (d.info.bitspersample / 8);
        const uint32_t batch_points = 176400 / bytes_per_point;
        while (d.position < d.info.noPoints) {
            if (callback) { callback(callback_parameter,
                static_cast<double>(d.position) * 100.0 / static_cast<double>(d.info.noPoints)); }
            const int32_t received = OptimFROG_read(&d, buffer.data(), batch_points, C_FALSE);
            if (received < 1) { return OptimFROG_FatalError; }
            const auto bytes = static_cast<std::size_t>(received) * bytes_per_point;
            if (std::fwrite(buffer.data(), 1, bytes, out.get()) != bytes) { return OptimFROG_WriteError; }
        }
        const int32_t tail_size = OptimFROG_readTail(&d, nullptr, 0);
        if (tail_size < 0) { return OptimFROG_FatalError; }
        std::vector<uint8_t> tail(static_cast<std::size_t>(tail_size));
        if (OptimFROG_readTail(&d, tail.data(), static_cast<uint32_t>(tail.size())) != tail_size) {
            return OptimFROG_FatalError;
        }
        if (!write_bytes(out.get(), tail)) { return OptimFROG_WriteError; }
        const bool recovered = d.audio->recoverable_errors();
        if (std::fclose(out.release()) != 0) { return OptimFROG_WriteError; }
        return recovered ? OptimFROG_RecoverableError : OptimFROG_NoError;
    } catch (const std::bad_alloc &) { return OptimFROG_MemoryError; }
    catch (...) { return OptimFROG_FatalError; }
}

} // extern "C"
