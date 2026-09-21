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

#if defined(ADBCPP_HAS_ZSTD)
// The compression level adb uses. The transfer is link-bound, so a faster, lower
// setting is the right trade, and it matches AOSP's `ZstdEncoder`.
constexpr int kCompressionLevel = 1;

std::string zstd_error(std::string_view operation, std::size_t code)
{
    return std::string(operation) + ": " + ZSTD_getErrorName(code);
}
#endif

#if defined(ADBCPP_HAS_LZ4)
std::string lz4_error(std::string_view operation, std::size_t code)
{
    return std::string(operation) + ": " + LZ4F_getErrorName(code);
}
#endif

#if defined(ADBCPP_HAS_BROTLI)
// The compression quality adb uses. The transfer is link-bound, so a faster,
// lower setting is the right trade, and it matches AOSP's `BrotliEncoder`.
constexpr int kBrotliQuality = 1;
#endif

} // namespace

#if defined(ADBCPP_HAS_ZSTD)
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
#endif

#if defined(ADBCPP_HAS_LZ4)
Result<std::vector<std::byte>> compress_lz4(std::span<const std::byte> input)
{
    // The block mode is independent, like AOSP's `Lz4Encoder`: a chunk is
    // compressed on its own, so a block cannot depend on an earlier chunk.
    LZ4F_preferences_t preferences{};
    preferences.frameInfo.blockMode = LZ4F_blockIndependent;

    std::vector<std::byte> output(LZ4F_compressFrameBound(input.size(), &preferences));
    const std::size_t size = LZ4F_compressFrame(output.data(), output.size(), input.data(), input.size(), &preferences);
    if (LZ4F_isError(size))
    {
        return tl::unexpected(Error{ErrorCode::Protocol, lz4_error("lz4 compression failed", size)});
    }
    output.resize(size);
    return output;
}

Lz4Decoder::Lz4Decoder()
    : decoder_(nullptr)
{
    LZ4F_dctx *context = nullptr;
    if (LZ4F_isError(LZ4F_createDecompressionContext(&context, LZ4F_VERSION)))
    {
        return;
    }
    decoder_.reset(context);
}

Result<std::vector<std::byte>> Lz4Decoder::decode(std::span<const std::byte> input)
{
    if (decoder_ == nullptr)
    {
        return tl::unexpected(Error{ErrorCode::Protocol, "the lz4 decompression context could not be created"});
    }

    // The output is appended across calls, because a frame can span chunks and a
    // chunk can hold more than one frame. `LZ4F_decompress` reports how much it
    // consumed and produced through the two sizes, and it has no input buffer of its
    // own, so the remaining input is passed back in.
    std::vector<std::byte> output;
    std::size_t offset = 0;
    while (offset < input.size())
    {
        std::array<std::byte, kMaxChunkSize> buffer{};
        std::size_t source_size = input.size() - offset;
        std::size_t destination_size = buffer.size();
        const std::size_t code = LZ4F_decompress(decoder_.get(), buffer.data(), &destination_size,
                                                 input.data() + offset, &source_size, nullptr);
        if (LZ4F_isError(code))
        {
            return tl::unexpected(Error{ErrorCode::Protocol, lz4_error("lz4 decompression failed", code)});
        }
        output.insert(output.end(), buffer.data(), buffer.data() + destination_size);
        offset += source_size;

        // A call that consumes nothing and produces nothing needs more input, so
        // the frame is not over; without this the loop would spin on it.
        if (source_size == 0 && destination_size == 0)
        {
            break;
        }
    }
    return output;
}
#endif

#if defined(ADBCPP_HAS_BROTLI)
Result<std::vector<std::byte>> compress_brotli(std::span<const std::byte> input)
{
    // `BrotliEncoderMaxCompressedSize` is the bound the encoder cannot exceed.
    // It returns zero only when the bound overflows `size_t`, which a chunk cannot,
    // so the fallback is never reached in practice.
    std::size_t size = BrotliEncoderMaxCompressedSize(input.size());
    if (size == 0)
    {
        size = input.size() + (input.size() / 4) + 1024;
    }

    std::vector<std::byte> output(size);
    std::size_t encoded_size = output.size();
    if (BrotliEncoderCompress(kBrotliQuality, BROTLI_DEFAULT_WINDOW, BROTLI_DEFAULT_MODE, input.size(),
                              reinterpret_cast<const std::uint8_t *>(input.data()), &encoded_size,
                              reinterpret_cast<std::uint8_t *>(output.data())) == BROTLI_FALSE)
    {
        return tl::unexpected(Error{ErrorCode::Protocol, "brotli compression failed"});
    }
    output.resize(encoded_size);
    return output;
}

BrotliDecoder::BrotliDecoder()
    : decoder_(BrotliDecoderCreateInstance(nullptr, nullptr, nullptr))
{
}

Result<std::vector<std::byte>> BrotliDecoder::decode(std::span<const std::byte> input)
{
    if (decoder_ == nullptr)
    {
        return tl::unexpected(Error{ErrorCode::Protocol, "the brotli decompression context could not be created"});
    }

    // The output is appended across calls, because a stream can span chunks and a
    // chunk can hold more than one stream. `BrotliDecoderDecompressStream` reports
    // how much it consumed and produced through the two sizes and keeps its own
    // input position, so the remaining input is passed back in.
    std::vector<std::byte> output;
    std::size_t available_in = input.size();
    const std::uint8_t *next_in = reinterpret_cast<const std::uint8_t *>(input.data());
    while (available_in > 0)
    {
        std::array<std::byte, kMaxChunkSize> buffer{};
        std::size_t available_out = buffer.size();
        std::uint8_t *next_out = reinterpret_cast<std::uint8_t *>(buffer.data());
        const std::size_t consumed_before = available_in;
        const BrotliDecoderResult result =
            BrotliDecoderDecompressStream(decoder_.get(), &available_in, &next_in, &available_out, &next_out, nullptr);
        if (result == BROTLI_DECODER_RESULT_ERROR)
        {
            return tl::unexpected(
                Error{ErrorCode::Protocol, std::string("brotli decompression failed: ") +
                                               BrotliDecoderErrorString(BrotliDecoderGetErrorCode(decoder_.get()))});
        }
        output.insert(output.end(), buffer.data(), buffer.data() + (buffer.size() - available_out));

        // A call that consumes nothing and produces nothing needs more input, so
        // the stream is not over; without this the loop would spin on it.
        if (available_in == consumed_before && available_out == buffer.size())
        {
            break;
        }
    }
    return output;
}
#endif

} // namespace adbcpp
