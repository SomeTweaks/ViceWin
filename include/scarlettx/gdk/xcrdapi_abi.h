#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#include <windows.h>
#if defined(SCARLETTX_BUILDING_XCRDAPI)
#define SCARLETTX_XCRDAPI_API __declspec(dllexport)
#else
#define SCARLETTX_XCRDAPI_API __declspec(dllimport)
#endif
#else
#define SCARLETTX_XCRDAPI_API
#define WINAPI
#endif

extern "C" {

using XDecompressResult = int32_t;

constexpr XDecompressResult XDECOMPRESS_S_OK = 0;
constexpr XDecompressResult XDECOMPRESS_E_INVALIDARG =
    static_cast<XDecompressResult>(0x80070057u);
constexpr XDecompressResult XDECOMPRESS_E_POINTER =
    static_cast<XDecompressResult>(0x80004003u);
constexpr XDecompressResult XDECOMPRESS_E_OUTOFMEMORY =
    static_cast<XDecompressResult>(0x8007000Eu);
constexpr XDecompressResult XDECOMPRESS_E_INVALID_DATA =
    static_cast<XDecompressResult>(0x8007000Du);
constexpr XDecompressResult XDECOMPRESS_E_INSUFFICIENT_BUFFER =
    static_cast<XDecompressResult>(0x8007007Au);
constexpr XDecompressResult XDECOMPRESS_E_NOTIMPL =
    static_cast<XDecompressResult>(0x80004001u);

enum XDecompressFormat : uint32_t {
    XDecompressFormat_RawDeflate = 0,
    XDecompressFormat_Zlib = 1,
};

struct XDecompressContext;
using XDecompressHandle = XDecompressContext*;

SCARLETTX_XCRDAPI_API XDecompressResult WINAPI XDecompressCreate(
    XDecompressFormat format,
    XDecompressHandle* context);
SCARLETTX_XCRDAPI_API XDecompressResult WINAPI XDecompress(
    XDecompressHandle context,
    const void* source,
    size_t source_size,
    void* destination,
    size_t destination_capacity,
    size_t* bytes_written);
SCARLETTX_XCRDAPI_API void WINAPI XDecompressDestroy(
    XDecompressHandle context);

}
