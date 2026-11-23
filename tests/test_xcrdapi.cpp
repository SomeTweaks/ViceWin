#include "sx_check.h"

#include "scarlettx/gdk/xcrdapi_abi.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include <zlib.h>

namespace {

std::vector<uint8_t> Compress(const std::string& input, int window_bits) {
    z_stream stream{};
    if (deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, window_bits, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        return {};
    }

    std::vector<uint8_t> output(compressBound(static_cast<uLong>(input.size())) + 32);
    stream.next_in = reinterpret_cast<Bytef*>(
        const_cast<char*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = output.data();
    stream.avail_out = static_cast<uInt>(output.size());
    const int result = deflate(&stream, Z_FINISH);
    if (result != Z_STREAM_END) {
        deflateEnd(&stream);
        return {};
    }
    output.resize(static_cast<size_t>(stream.total_out));
    deflateEnd(&stream);
    return output;
}

bool RoundTrip(XDecompressFormat format, int window_bits,
               const std::string& expected) {
    const std::vector<uint8_t> compressed = Compress(expected, window_bits);
    if (compressed.empty()) {
        return false;
    }

    XDecompressHandle context = nullptr;
    if (XDecompressCreate(format, &context) != XDECOMPRESS_S_OK || !context) {
        return false;
    }

    std::vector<char> decoded(expected.size() + 8, '\0');
    size_t written = 0;
    const XDecompressResult result = XDecompress(
        context, compressed.data(), compressed.size(), decoded.data(),
        decoded.size(), &written);
    XDecompressDestroy(context);
    return result == XDECOMPRESS_S_OK && written == expected.size() &&
           std::equal(expected.begin(), expected.end(), decoded.begin());
}

}  // namespace

int main() {
    const std::string expected =
        "Xbox title resource decompression through the xcrdapi HLE shim.";
    SX_REQUIRE(RoundTrip(XDecompressFormat_Zlib, MAX_WBITS, expected));
    SX_REQUIRE(RoundTrip(XDecompressFormat_RawDeflate, -MAX_WBITS, expected));

    const std::vector<uint8_t> compressed =
        Compress(expected, MAX_WBITS);
    SX_REQUIRE(!compressed.empty());
    XDecompressHandle context = nullptr;
    SX_REQUIRE(XDecompressCreate(XDecompressFormat_Zlib, &context) ==
               XDECOMPRESS_S_OK);
    std::vector<char> too_small(expected.size() - 1);
    size_t written = 123;
    SX_REQUIRE(XDecompress(context, compressed.data(), compressed.size(),
                           too_small.data(), too_small.size(), &written) ==
               XDECOMPRESS_E_INSUFFICIENT_BUFFER);
    SX_REQUIRE(written == 0);

    const uint8_t invalid_stream[] = {0xFF, 0x00, 0x13};
    SX_REQUIRE(XDecompress(context, invalid_stream, sizeof(invalid_stream),
                           too_small.data(), too_small.size(), &written) ==
               XDECOMPRESS_E_INVALID_DATA);
    XDecompressDestroy(context);

    std::cout << "[test_xcrdapi] PASSED (zlib and raw DEFLATE)\n";
    return 0;
}
