#include "codec.h"
#include <OptimFROG.h>
namespace ttp::ofr {
static_assert(sizeof(ReadInterface) == 28 && sizeof(OptimFROG_Info) == 68 && sizeof(condition_t) == 1,
    "OpenFROG preserves the packed x86 SDK structures and byte-sized conditions");
struct Decoder::Impl {
    void* context{};
    Input* input{};
    uint64_t position{};
    HRESULT error{S_OK};
    bool flip_sign{};
    ~Impl() { if (context) OptimFROG_destroyInstance(context); }
    template<class F> static int64_t call(void* id, F f, int64_t failure = -1) noexcept {
        auto& s = *static_cast<Impl*>(id); int64_t result{};
        auto hr = protect([&] { result = f(s); return S_OK; });
        if (FAILED(hr)) { if (SUCCEEDED(s.error)) s.error = hr; return failure; }
        return result;
    }
    static condition_t close(void*) { return C_TRUE; }
    static sInt32_t read(void* id, void* data, uInt32_t n) {
        return sInt32_t(call(id, [&](Impl& s) {
            require(n <= INT_MAX, E_INVALIDARG); ULONG done{};
            while (done < n) {
                require(WaitForSingleObject(s.input->event, 0) != WAIT_OBJECT_0, E_ABORT);
                auto got = s.input->read(static_cast<BYTE*>(data) + done, n - done);
                done += got; s.position += got; if (!got) break;
            }
            return int64_t(done);
        }));
    }
    static condition_t eof(void* id) { auto& s = *static_cast<Impl*>(id); return s.position >= s.input->size - s.input->base ? C_TRUE : C_FALSE; }
    static condition_t seekable(void*) { return C_TRUE; }
    static sInt64_t length(void* id) { auto& s = *static_cast<Impl*>(id); return sInt64_t(s.input->size - s.input->base); }
    static sInt64_t pos(void* id) { return sInt64_t(static_cast<Impl*>(id)->position); }
    static condition_t at(void* id, sInt64_t p) {
        return condition_t(call(id, [&](Impl& s) {
            require(p >= 0 && uint64_t(p) <= s.input->size - s.input->base, STG_E_SEEKERROR);
            s.input->at(s.input->base + uint64_t(p)); s.position = uint64_t(p); return 1;
        }, 0));
    }
};
Decoder::Decoder() = default;
Decoder::~Decoder() = default;
void Decoder::open(Input& in) {
    impl_ = std::make_unique<Impl>(); auto& s = *impl_; s.input = &in;
    in.at(0); in.signature("OFR ", "OFRX");
    BYTE signature[4]{}; in.exact(signature, 4); in.at(in.base);
    // The statically linked backend retains the integer-only SDK scope.
    // FLOAT32 identifiers in a header do not establish OFRX audio support.
    require(memcmp(signature, "OFRX", 4) != 0, HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
    static ReadInterface callbacks{Impl::close, Impl::read, Impl::eof, Impl::seekable, Impl::length, Impl::pos, Impl::at};
    s.context = OptimFROG_createInstance(); require(s.context, E_OUTOFMEMORY);
    auto ok = OptimFROG_openExt(s.context, &callbacks, &s, C_FALSE); check(s.error); require(ok != C_FALSE);
    OptimFROG_Info info{}; require(OptimFROG_getInfo(s.context, &info) != C_FALSE); check(s.error);
    require(info.channels >= 1 && info.channels <= 2 && info.samplerate && info.noPoints >= 0 &&
        info.bitspersample >= 8 && info.bitspersample <= 32 && !(info.bitspersample % 8) && info.sampleType);
    const bool floating = !strncmp(info.sampleType, "FLOAT32", 7);
    const bool unsigned_pcm = !strncmp(info.sampleType, "UINT", 4);
    const bool signed_pcm = !strncmp(info.sampleType, "SINT", 4);
    require(floating || unsigned_pcm || signed_pcm, HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
    require(!floating || info.bitspersample == 32);
    const auto align = info.channels * (info.bitspersample / 8);
    require(uint64_t(info.samplerate) * align <= MAXDWORD);
    format = {WORD(floating ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM), WORD(info.channels), info.samplerate,
        info.samplerate * align, WORD(align), WORD(info.bitspersample), 0};
    valid_bits = WORD(info.bitspersample); channel_mask = info.channels == 1 ? 4 : 3;
    total = uint64_t(info.noPoints); bitrate = dword(uint64_t(info.bitrate) * 1000);
    s.flip_sign = !floating && (info.bitspersample == 8 ? signed_pcm : unsigned_pcm);
    // The SDK reports container width. Recover valid bits only from an actual
    // WAVE EXTENSIBLE header; never infer them by scanning silent/zero samples.
    const auto head_size = OptimFROG_readHead(s.context, nullptr, 0); check(s.error);
    require(head_size >= 0);
    if (head_size > 0 && head_size <= 1024 * 1024) {
        Bytes h(size_t(head_size), 0); require(OptimFROG_readHead(s.context, h.data(), DWORD(h.size())) == head_size); check(s.error);
        if (h.size() >= 12 && !memcmp(h.data(), "RIFF", 4) && !memcmp(h.data() + 8, "WAVE", 4)) {
            for (size_t p = 12; p + 8 <= h.size();) {
                const auto n = le32(h.data() + p + 4); const auto body = p + 8;
                if (n > h.size() - body) break;
                if (!memcmp(h.data() + p, "fmt ", 4) && n >= 40 && le16(h.data() + body) == WAVE_FORMAT_EXTENSIBLE) {
                    const auto bits = le16(h.data() + body + 18);
                    const auto mask = le32(h.data() + body + 20);
                    if (!floating && bits && bits <= valid_bits) valid_bits = bits;
                    unsigned count{}; for (auto v = mask; v; v &= v - 1) ++count;
                    if (count == info.channels) channel_mask = mask;
                    break;
                }
                p = body + n + (n & 1);
            }
        }
    }
}
DWORD Decoder::read(BYTE* out, DWORD frames) {
    auto& s = *impl_; auto got = OptimFROG_read(s.context, out, frames, C_FALSE); check(s.error);
    require(got >= 0 && DWORD(got) <= frames); require(!OptimFROG_recoverableErrors(s.context), HRESULT_FROM_WIN32(ERROR_CRC));
    // WAVE PCM is unsigned only at 8 bit. XOR is exact, including 32-bit LSBs.
    if (s.flip_sign) {
        const auto bytes = format.wBitsPerSample / 8;
        for (size_t i = 0; i < size_t(got) * format.nChannels; ++i) out[i * bytes + bytes - 1] ^= 128;
    }
    return DWORD(got);
}
void Decoder::seek(uint64_t frame) {
    auto& s = *impl_; s.error = S_OK;
    const auto ok = OptimFROG_seekPoint(s.context, sInt64_t(frame)); check(s.error); require(ok != C_FALSE, STG_E_SEEKERROR);
}
}
