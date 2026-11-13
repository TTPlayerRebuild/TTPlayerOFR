#ifndef OPENFROG_OPTIMFROG_H
#define OPENFROG_OPTIMFROG_H

#include <stdint.h>

#if defined(_WIN32)
#define OFR_CALL __cdecl
#if defined(OPENFROG_BUILD) && defined(OPENFROG_USE_DEF)
#define OFR_API
#elif defined(OPENFROG_BUILD)
#define OFR_API __declspec(dllexport)
#elif defined(OPENFROG_DYNAMIC_API)
#define OFR_API
#else
#define OFR_API __declspec(dllimport)
#endif
#else
#define OFR_CALL
#define OFR_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#define OFR_DEFAULT(value) = value
#else
#define OFR_DEFAULT(value)
#endif

typedef uint8_t condition_t;
typedef int32_t sInt32_t;
typedef uint32_t uInt32_t;
typedef int64_t sInt64_t;
typedef double Float64_t;

#define C_FALSE ((condition_t)0)
#define C_TRUE ((condition_t)1)
#ifndef C_NULL
#ifdef __cplusplus
#define C_NULL 0
#else
#define C_NULL ((void *)0)
#endif
#endif

/* SDK 1.600 ABI: pack(1) matters even for 32-bit builds. */
#pragma pack(push, 1)
typedef struct ReadInterface {
    condition_t (OFR_CALL *close)(void *instance);
    sInt32_t (OFR_CALL *read)(void *instance, void *destBuffer, uInt32_t count);
    condition_t (OFR_CALL *eof)(void *instance);
    condition_t (OFR_CALL *seekable)(void *instance);
    sInt64_t (OFR_CALL *length)(void *instance);
    sInt64_t (OFR_CALL *getPos)(void *instance);
    condition_t (OFR_CALL *seek)(void *instance, sInt64_t pos);
} ReadInterface;

typedef struct OptimFROG_Tags {
    uInt32_t keyCount;
    char *keys[64];
    char *values[64];
} OptimFROG_Tags;

typedef struct OptimFROG_Info {
    uInt32_t channels;
    uInt32_t samplerate;
    uInt32_t bitspersample;
    uInt32_t bitrate;
    uInt32_t version;
    const char *method;
    const char *speedup;
    sInt64_t noPoints;
    sInt64_t originalSize;
    sInt64_t compressedSize;
    sInt64_t length_ms;
    const char *sampleType;
    const char *channelConfig;
} OptimFROG_Info;
#pragma pack(pop)

typedef void (OFR_CALL *OptimFROG_callBack)(void *callBackParam, Float64_t percentage);

#define OptimFROG_NoError 0
#define OptimFROG_MemoryError 1
#define OptimFROG_OpenError 2
#define OptimFROG_WriteError 3
#define OptimFROG_FatalError 4
#define OptimFROG_RecoverableError 5

OFR_API uInt32_t OFR_CALL OptimFROG_getVersion(void);
OFR_API void *OFR_CALL OptimFROG_createInstance(void);
OFR_API void OFR_CALL OptimFROG_destroyInstance(void *decoderInstance);
OFR_API condition_t OFR_CALL OptimFROG_openExt(void *decoderInstance, ReadInterface *rInt,
    void *readerInstance, condition_t readTags OFR_DEFAULT(C_FALSE));
OFR_API condition_t OFR_CALL OptimFROG_open(void *decoderInstance, char *fileName,
    condition_t readTags OFR_DEFAULT(C_FALSE));
OFR_API condition_t OFR_CALL OptimFROG_close(void *decoderInstance);
OFR_API sInt32_t OFR_CALL OptimFROG_readHead(void *decoderInstance, void *headData, uInt32_t maxSize);
OFR_API sInt32_t OFR_CALL OptimFROG_readTail(void *decoderInstance, void *tailData, uInt32_t maxSize);
OFR_API condition_t OFR_CALL OptimFROG_seekable(void *decoderInstance);
OFR_API condition_t OFR_CALL OptimFROG_seekPoint(void *decoderInstance, sInt64_t point);
OFR_API condition_t OFR_CALL OptimFROG_seekTime(void *decoderInstance, sInt64_t milliseconds);
OFR_API sInt64_t OFR_CALL OptimFROG_getPos(void *decoderInstance);
OFR_API condition_t OFR_CALL OptimFROG_recoverableErrors(void *decoderInstance);
OFR_API sInt32_t OFR_CALL OptimFROG_read(void *decoderInstance, void *data, uInt32_t noPoints,
    condition_t max16bit OFR_DEFAULT(C_FALSE));
OFR_API condition_t OFR_CALL OptimFROG_getInfo(void *decoderInstance, OptimFROG_Info *info);
OFR_API condition_t OFR_CALL OptimFROG_getTags(void *decoderInstance, OptimFROG_Tags *tags);
OFR_API void OFR_CALL OptimFROG_freeTags(OptimFROG_Tags *tags);
OFR_API sInt32_t OFR_CALL OptimFROG_decodeFile(char *sourceFile, char *destinationFile,
    OptimFROG_callBack callBack OFR_DEFAULT(0), void *callBackParam OFR_DEFAULT(0));
OFR_API sInt32_t OFR_CALL OptimFROG_infoFile(char *sourceFile, OptimFROG_Info *info,
    OptimFROG_Tags *tags OFR_DEFAULT(0));

#undef OFR_DEFAULT
#ifdef __cplusplus
}
static_assert(sizeof(condition_t) == 1, "SDK boolean must occupy one byte");
static_assert(sizeof(OptimFROG_Info) == 52 + 4 * sizeof(void *), "SDK Info packing mismatch");
static_assert(sizeof(OptimFROG_Tags) == 4 + 128 * sizeof(void *), "SDK Tags packing mismatch");
static_assert(sizeof(ReadInterface) == 7 * sizeof(void *), "SDK Reader packing mismatch");
#endif

#endif
