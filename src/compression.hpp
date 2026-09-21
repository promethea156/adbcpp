#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

#include "adbcpp/error.hpp"

#if defined(ADBCPP_HAS_LZ4)
#    include <lz4frame.h>
#endif

#if defined(ADBCPP_HAS_ZSTD)
#    include <zstd.h>
#endif

namespace adbcpp
{

#if defined(ADBCPP_HAS_ZSTD)
/// Compresses one sync chunk as a complete zstd frame.
///
/// A `DATA` packet carries one chunk, and a complete frame per chunk is a valid
/// zstd stream: the device decodes with `ZSTD_decompressStream`, which starts the
/// next frame when it reaches the end of one. This is why the encoder is not a
/// stream: its output would span chunks, which the per-chunk framing here cannot
/// express. AOSP's `ZstdEncoder` streams the whole transfer, so its frames are
/// larger; both are decodable by the other side.
Result<std::vector<std::byte>> compress_zstd(std::span<const std::byte> input);

/// Decompresses a zstd stream incrementally, across `DATA` chunks.
///
/// The device's encoder streams the whole transfer as one frame and splits its
/// output into `DATA` chunks at arbitrary points, so a chunk boundary is not
/// necessarily a frame boundary. `ZSTD_decompressStream` is therefore called per
/// chunk and its output is appended, exactly like AOSP's `ZstdDecoder`.
class ZstdDecoder
{
public:
    ZstdDecoder();
    ~ZstdDecoder() = default;
    ZstdDecoder(ZstdDecoder &&) noexcept = default;
    ZstdDecoder &operator=(ZstdDecoder &&) noexcept = default;
    ZstdDecoder(const ZstdDecoder &) = delete;
    ZstdDecoder &operator=(const ZstdDecoder &) = delete;

    /// Feeds the next `DATA` chunk and returns the bytes it decompresses to.
    Result<std::vector<std::byte>> decode(std::span<const std::byte> input);

private:
    struct Deleter
    {
        void operator()(ZSTD_DStream *stream) const noexcept
        {
            ZSTD_freeDStream(stream);
        }
    };

    std::unique_ptr<ZSTD_DStream, Deleter> decoder_;
};
#endif

#if defined(ADBCPP_HAS_LZ4)
/// Compresses one sync chunk as a complete lz4 frame.
///
/// This is the frame API (`LZ4F_*`), not the block API, exactly like AOSP's
/// `Lz4Encoder`: a block has no framing of its own, so the device's
/// `LZ4F_decompress` would not know where a chunk ends. As with zstd, a complete
/// frame per chunk is a valid stream because the device's decoder starts the next
/// frame at the end of one.
Result<std::vector<std::byte>> compress_lz4(std::span<const std::byte> input);

/// Decompresses an lz4 stream incrementally, across `DATA` chunks.
///
/// The device's encoder streams the whole transfer as one frame and splits its
/// output into `DATA` chunks at arbitrary points, so a chunk boundary is not
/// necessarily a frame boundary. `LZ4F_decompress` is therefore called per chunk
/// and its output is appended, exactly like AOSP's `Lz4Decoder`.
class Lz4Decoder
{
public:
    Lz4Decoder();
    ~Lz4Decoder() = default;
    Lz4Decoder(Lz4Decoder &&) noexcept = default;
    Lz4Decoder &operator=(Lz4Decoder &&) noexcept = default;
    Lz4Decoder(const Lz4Decoder &) = delete;
    Lz4Decoder &operator=(const Lz4Decoder &) = delete;

    /// Feeds the next `DATA` chunk and returns the bytes it decompresses to.
    Result<std::vector<std::byte>> decode(std::span<const std::byte> input);

private:
    struct Deleter
    {
        void operator()(LZ4F_dctx *context) const noexcept
        {
            LZ4F_freeDecompressionContext(context);
        }
    };

    std::unique_ptr<LZ4F_dctx, Deleter> decoder_;
};
#endif

} // namespace adbcpp
