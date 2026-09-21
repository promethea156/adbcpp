#pragma once

#include <zstd.h>

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

#include "adbcpp/error.hpp"

namespace adbcpp
{

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

} // namespace adbcpp
