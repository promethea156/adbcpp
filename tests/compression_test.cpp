#if defined(ADBCPP_HAS_ZSTD) || defined(ADBCPP_HAS_LZ4)

#    include "compression.hpp"

#    include <catch2/catch_test_macros.hpp>
#    include <cstddef>
#    include <span>
#    include <string>

namespace
{

std::span<const std::byte> bytes_of(const std::string &text)
{
    return std::span(reinterpret_cast<const std::byte *>(text.data()), text.size());
}

std::string text_of(std::span<const std::byte> bytes)
{
    return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

} // namespace

#    if defined(ADBCPP_HAS_ZSTD)
TEST_CASE("zstd round-trips a compressible buffer", "[compression]")
{
    // A buffer with repetition compresses to less than it started with.
    std::string text;
    for (int i = 0; i < 1024; ++i)
    {
        text += "hello world ";
    }

    const auto compressed = adbcpp::compress_zstd(bytes_of(text));
    REQUIRE(compressed.has_value());
    REQUIRE(compressed->size() < text.size());

    adbcpp::ZstdDecoder decoder;
    const auto decoded = decoder.decode(*compressed);
    REQUIRE(decoded.has_value());
    REQUIRE(text_of(*decoded) == text);
}

TEST_CASE("zstd round-trips a buffer that does not compress", "[compression]")
{
    // Incompressible bytes come back unchanged, and the frame may be larger than
    // the input, which is why the compressed size is not capped at the chunk size.
    std::string text;
    for (int i = 0; i < 4096; ++i)
    {
        text.push_back(static_cast<char>(i * 31 + 7));
    }

    const auto compressed = adbcpp::compress_zstd(bytes_of(text));
    REQUIRE(compressed.has_value());

    adbcpp::ZstdDecoder decoder;
    const auto decoded = decoder.decode(*compressed);
    REQUIRE(decoded.has_value());
    REQUIRE(text_of(*decoded) == text);
}

TEST_CASE("zstd decoder reads a frame split across chunks", "[compression]")
{
    std::string text(4096, 'a');
    const auto compressed = adbcpp::compress_zstd(bytes_of(text));
    REQUIRE(compressed.has_value());

    // The frame is fed in two pieces, so a chunk boundary is not a frame
    // boundary, exactly like the device's streaming encoder.
    const auto middle = compressed->size() / 2;
    adbcpp::ZstdDecoder decoder;
    const auto first = decoder.decode(std::span(compressed->data(), middle));
    REQUIRE(first.has_value());
    const auto second = decoder.decode(std::span(compressed->data() + middle, compressed->size() - middle));
    REQUIRE(second.has_value());

    std::string decoded = text_of(*first);
    decoded += text_of(*second);
    REQUIRE(decoded == text);
}
#    endif

#    if defined(ADBCPP_HAS_LZ4)
TEST_CASE("lz4 round-trips a compressible buffer", "[compression]")
{
    std::string text;
    for (int i = 0; i < 1024; ++i)
    {
        text += "hello world ";
    }

    const auto compressed = adbcpp::compress_lz4(bytes_of(text));
    REQUIRE(compressed.has_value());
    REQUIRE(compressed->size() < text.size());

    adbcpp::Lz4Decoder decoder;
    const auto decoded = decoder.decode(*compressed);
    REQUIRE(decoded.has_value());
    REQUIRE(text_of(*decoded) == text);
}

TEST_CASE("lz4 decoder reads a frame split across chunks", "[compression]")
{
    std::string text(4096, 'a');
    const auto compressed = adbcpp::compress_lz4(bytes_of(text));
    REQUIRE(compressed.has_value());

    // The frame is fed in two pieces, so a chunk boundary is not a frame
    // boundary, exactly like the device's streaming encoder.
    const auto middle = compressed->size() / 2;
    adbcpp::Lz4Decoder decoder;
    const auto first = decoder.decode(std::span(compressed->data(), middle));
    REQUIRE(first.has_value());
    const auto second = decoder.decode(std::span(compressed->data() + middle, compressed->size() - middle));
    REQUIRE(second.has_value());

    std::string decoded = text_of(*first);
    decoded += text_of(*second);
    REQUIRE(decoded == text);
}
#    endif

#endif
