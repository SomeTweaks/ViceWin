#include "scarlettx/gdk/xcrdapi_abi.h"

#include "core/logger.h"

#include <limits>
#include <mutex>
#include <new>
#include <string>

#if SCARLETTX_HAS_ZLIB
#include <zlib.h>
constexpr size_t kMaxSingleStreamSize =
    static_cast<size_t>(std::numeric_limits<uInt>::max());
#endif

struct XDecompressContext {
    XDecompressFormat format = XDecompressFormat_Zlib;
    std::mutex mutex;
#if SCARLETTX_HAS_ZLIB
    z_stream stream{};
    bool initialized = false;
#endif
};

namespace {

XDecompressResult InvalidData(const char* detail) {
    scarlettx::LogMessage(scarlettx::LogLevel::Error, "xcrdapi",
                          std::string("XDecompress failed: ") + detail);
    return XDECOMPRESS_E_INVALID_DATA;
}

}  // namespace

extern "C" {

XDecompressResult WINAPI XDecompressCreate(
    XDecompressFormat format,
    XDecompressHandle* context) {
    if (!context) {
        return XDECOMPRESS_E_POINTER;
    }
    *context = nullptr;
    if (format != XDecompressFormat_RawDeflate &&
        format != XDecompressFormat_Zlib) {
        return XDECOMPRESS_E_INVALIDARG;
    }
#if !SCARLETTX_HAS_ZLIB
    scarlettx::LogMessage(scarlettx::LogLevel::Error, "xcrdapi",
                          "XDecompressCreate requires zlib support");
    return XDECOMPRESS_E_NOTIMPL;
#else
    auto* created = new (std::nothrow) XDecompressContext();
    if (!created) {
        return XDECOMPRESS_E_OUTOFMEMORY;
    }
    created->format = format;
    const int window_bits = format == XDecompressFormat_RawDeflate
                                ? -MAX_WBITS
                                : MAX_WBITS;
    const int result = inflateInit2(&created->stream, window_bits);
    if (result != Z_OK) {
        delete created;
        scarlettx::LogMessage(scarlettx::LogLevel::Error, "xcrdapi",
                              "inflateInit2 failed while creating a decompressor");
        return result == Z_MEM_ERROR ? XDECOMPRESS_E_OUTOFMEMORY
                                     : XDECOMPRESS_E_INVALIDARG;
    }
    created->initialized = true;
    *context = created;
    return XDECOMPRESS_S_OK;
#endif
}

XDecompressResult WINAPI XDecompress(
    XDecompressHandle context,
    const void* source,
    size_t source_size,
    void* destination,
    size_t destination_capacity,
    size_t* bytes_written) {
    if (bytes_written) {
        *bytes_written = 0;
    }
    if (!context || !bytes_written ||
        (!source && source_size != 0) ||
        (!destination && destination_capacity != 0)) {
        return XDECOMPRESS_E_POINTER;
    }
#if !SCARLETTX_HAS_ZLIB
    (void)source_size;
    (void)destination_capacity;
    return XDECOMPRESS_E_NOTIMPL;
#else
    if (!context->initialized) {
        return XDECOMPRESS_E_INVALIDARG;
    }
    if (source_size > kMaxSingleStreamSize ||
        destination_capacity > kMaxSingleStreamSize) {
        return XDECOMPRESS_E_INVALIDARG;
    }

    std::lock_guard<std::mutex> lock(context->mutex);
    if (inflateReset(&context->stream) != Z_OK) {
        return InvalidData("inflateReset failed");
    }

    Bytef empty_output = 0;
    context->stream.next_in = const_cast<Bytef*>(
        static_cast<const Bytef*>(source));
    context->stream.avail_in = static_cast<uInt>(source_size);
    context->stream.next_out = destination
                                   ? static_cast<Bytef*>(destination)
                                   : &empty_output;
    context->stream.avail_out = destination_capacity == 0
                                    ? 1u
                                    : static_cast<uInt>(destination_capacity);

    int result = Z_OK;
    do {
        result = inflate(&context->stream, Z_FINISH);
    } while (result == Z_OK);

    const size_t produced =
        static_cast<size_t>(context->stream.total_out);
    if (result == Z_STREAM_END) {
        if (produced > destination_capacity) {
            return XDECOMPRESS_E_INSUFFICIENT_BUFFER;
        }
        *bytes_written = produced;
        return XDECOMPRESS_S_OK;
    }
    if (result == Z_BUF_ERROR && context->stream.avail_out == 0) {
        return XDECOMPRESS_E_INSUFFICIENT_BUFFER;
    }
    return InvalidData(result == Z_NEED_DICT
                           ? "stream requires an unsupported preset dictionary"
                           : "input is truncated or is not a valid DEFLATE stream");
#endif
}

void WINAPI XDecompressDestroy(XDecompressHandle context) {
    if (!context) {
        return;
    }
#if SCARLETTX_HAS_ZLIB
    if (context->initialized) {
        inflateEnd(&context->stream);
        context->initialized = false;
    }
#endif
    delete context;
}

}  // extern "C"
