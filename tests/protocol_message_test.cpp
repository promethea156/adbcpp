#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <catch2/catch_test_macros.hpp>

#include "adbcpp/protocol/commands.hpp"
#include "adbcpp/protocol/message.hpp"

using adbcpp::protocol::Message;

TEST_CASE("message header round-trips through encode/decode", "[protocol]") {
  Message original;
  original.command = adbcpp::protocol::kCnxn;
  original.arg0 = 0x01000001u;
  original.arg1 = 4096u;
  original.data_length = 0u;
  original.data_crc32 = 0u;
  original.magic = Message::compute_magic(original.command);

  const auto encoded = original.encode();
  const auto decoded = Message::decode(encoded);

  REQUIRE(decoded.command == original.command);
  REQUIRE(decoded.arg0 == original.arg0);
  REQUIRE(decoded.arg1 == original.arg1);
  REQUIRE(decoded.data_length == original.data_length);
  REQUIRE(decoded.data_crc32 == original.data_crc32);
  REQUIRE(decoded.magic == original.magic);
}

TEST_CASE("message header is serialized little-endian", "[protocol]") {
  Message message;
  message.command = adbcpp::protocol::kCnxn;
  message.magic = Message::compute_magic(message.command);

  const auto bytes = message.encode();

  REQUIRE(bytes.size() == adbcpp::protocol::kMessageHeaderSize);
  REQUIRE(bytes[0] == std::byte{'C'});
  REQUIRE(bytes[1] == std::byte{'N'});
  REQUIRE(bytes[2] == std::byte{'X'});
  REQUIRE(bytes[3] == std::byte{'N'});
}

TEST_CASE("magic is the bitwise inverse of command", "[protocol]") {
  REQUIRE(Message::compute_magic(0u) == 0xFFFFFFFFu);
  REQUIRE(Message::compute_magic(0xFFFFFFFFu) == 0u);
  REQUIRE(Message::compute_magic(adbcpp::protocol::kCnxn) ==
          (adbcpp::protocol::kCnxn ^ 0xFFFFFFFFu));
}

TEST_CASE("crc32 matches the standard test vector", "[protocol]") {
  const char *text = "123456789";
  const auto data =
      std::span(reinterpret_cast<const std::byte *>(text), 9);

  REQUIRE(Message::compute_crc32(data) == 0xCBF43926u);
}
