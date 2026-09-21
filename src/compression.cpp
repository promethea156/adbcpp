#include "compression.hpp"

#include <array>
#include <string>

namespace adbcpp
{
namespace
{

// A decompressed `DATA` chunk is at most `SYNC_DATA_MAX`, the same cap the sync
// protocol puts on a chunk (see `src/sync.cpp`), so one output buffer per call is
// enough for a chunk and a larger stream only needs more calls, not more buffer.
constexpr std::size_t kMaxChunkSize = 64 * 1024;

// The compression level adb uses. The transfer is link-bound, so a faster, lower
// setting is the right trade, and it matches AOSP's `ZstdEncoder`.
constexpr int kCompressionLevel = 1;

std::string zstd_error(std::string_view operation, std::size_t code)
{
    return std::string(operation) + ": " + ZSTD_getErrorName(code);
}

} // namespace

Result<std::vector<std::byte>> compress_zstd(std::span<const std::byte> input)
{
    std::vector<std::byte> output(ZSTD_compressBound(input.size()));
    const std::size_t size = ZSTD_compress(output.data(), output.size(), input.data(), input.size(), kCompressionLevel);
    if (ZSTD_isError(size))
    {
        return tl::unexpected(Error{ErrorCode::Protocol, zstd_error("zstd compression failed", size)});
    }
    output.resize(size);
    return output;
}

ZstdDecoder::ZstdDecoder()
    : decoder_(ZSTD_createDStream())
{
    if (decoder_ != nullptr && ZSTD_isError(ZSTD_initDStream(decoder_.get())))
    {
        decoder_.reset();
    }
}

Result<std::vector<std::byte>> ZstdDecoder::decode(std::span<const std::byte> input)
{
    if (decoder_ == nullptr)
    {
        return tl::unexpected(Error{ErrorCode::Protocol, "the zstd decompression context could not be created"});
    }

    // The output is appended across calls, because a frame can span chunks and a
    // chunk can hold more than one frame.
    std::vector<std::byte> output;
    ZSTD_inBuffer in{input.data(), input.size(), 0};
    while (in.pos < in.size)
    {
        std::array<std::byte, kMaxChunkSize> buffer{};
        ZSTD_outBuffer out{buffer.data(), buffer.size(), 0};
        const std::size_t code = ZSTD_decompressStream(decoder_.get(), &out, &in);
        if (ZSTD_isError(code))
        {
            return tl::unexpected(Error{ErrorCode::Protocol, zstd_error("zstd decompression failed", code)});
        }
        output.insert(output.end(), buffer.data(), buffer.data() + out.pos);

        // A call that consumes nothing and produces nothing needs more input, so
        // the frame is not over; without this the loop would spin on it.
        if (in.pos == 0 && out.pos == 0)
        {
            break;
        }
    }
    return output;
}

} // namespace adbcpp
