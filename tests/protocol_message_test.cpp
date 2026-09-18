#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>

#include "adbcpp/protocol/commands.hpp"
#include "adbcpp/protocol/message.hpp"

// These tests pin down the wire format of the 24-byte ADB header described in
// `docs/dev/protocol.md`: six little-endian 32-bit words, `magic` as the inverse
// of `command`, and a payload byte sum in the field the document calls
// `data_crc32`.
using adbcpp::protocol::Message;

TEST_CASE("message header round-trips through encode/decode", "[protocol]")
{
    Message original;
    original.command = adbcpp::protocol::kCnxn;
    original.arg0 = 0x01000001u;
    original.arg1 = 4096u;
    original.data_length = 0u;
    original.data_check = 0u;
    original.magic = Message::compute_magic(original.command);

    const auto encoded = original.encode();
    const auto decoded = Message::decode(encoded);

    REQUIRE(decoded.command == original.command);
    REQUIRE(decoded.arg0 == original.arg0);
    REQUIRE(decoded.arg1 == original.arg1);
    REQUIRE(decoded.data_length == original.data_length);
    REQUIRE(decoded.data_check == original.data_check);
    REQUIRE(decoded.magic == original.magic);
}

TEST_CASE("message header is serialized little-endian", "[protocol]")
{
    Message message;
    message.command = adbcpp::protocol::kCnxn;
    message.magic = Message::compute_magic(message.command);

    const auto bytes = message.encode();

    // A little-endian `CNXN` is the ASCII bytes in reading order, which is why
    // the command constants are built with `make_command`.
    REQUIRE(bytes.size() == adbcpp::protocol::kMessageHeaderSize);
    REQUIRE(bytes[0] == std::byte{'C'});
    REQUIRE(bytes[1] == std::byte{'N'});
    REQUIRE(bytes[2] == std::byte{'X'});
    REQUIRE(bytes[3] == std::byte{'N'});
}

TEST_CASE("magic is the bitwise inverse of command", "[protocol]")
{
    REQUIRE(Message::compute_magic(0u) == 0xFFFFFFFFu);
    REQUIRE(Message::compute_magic(0xFFFFFFFFu) == 0u);
    REQUIRE(Message::compute_magic(adbcpp::protocol::kCnxn) == (adbcpp::protocol::kCnxn ^ 0xFFFFFFFFu));
}

TEST_CASE("checksum is the sum of the payload bytes", "[protocol]")
{
    // Despite the field's name, adb's `calculate_apacket_checksum` adds the
    // payload bytes rather than computing a CRC-32. adbd verifies the CNXN and
    // AUTH checksum, so this is the value the handshake must carry.
    const char *text = "123456789";
    const auto data = std::span(reinterpret_cast<const std::byte *>(text), 9);

    REQUIRE(Message::compute_checksum(data) == 0x1DDu);
    REQUIRE(Message::compute_checksum({}) == 0u);
}

TEST_CASE("data_length_of reports the payload size as a 32-bit length", "[protocol]")
{
    const std::array<std::byte, 3> payload{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};

    REQUIRE(*Message::data_length_of(payload) == 3u);
    REQUIRE(*Message::data_length_of({}) == 0u);
}
