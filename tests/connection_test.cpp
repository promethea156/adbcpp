#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "adbcpp/connection.hpp"
#include "adbcpp/protocol/commands.hpp"
#include "adbcpp/protocol/message.hpp"
#include "adbcpp/testing/mock_transport.hpp"

using adbcpp::protocol::Message;

namespace {

adbcpp::protocol::Message make_message(std::uint32_t command,
                                        std::uint32_t arg0,
                                        std::uint32_t arg1,
                                        std::span<const std::byte> payload = {}) {
  adbcpp::protocol::Message message;
  message.command = command;
  message.arg0 = arg0;
  message.arg1 = arg1;
  message.data_length = payload.size();
  message.data_crc32 = Message::compute_crc32(payload);
  message.magic = Message::compute_magic(command);
  return message;
}

} // namespace

TEST_CASE("connection completes the handshake without authentication",
          "[connection]") {
  adbcpp::testing::MockTransport transport;

  const auto device = make_message(adbcpp::protocol::kCnxn, 0x01000001u, 4096u);
  transport.feed(device.encode());

  adbcpp::Connection connection(transport);

  REQUIRE(connection.device_version() == 0x01000001u);
  REQUIRE(connection.max_data() == 4096u);
}

TEST_CASE("connection answers an AUTH request with the public key",
          "[connection]") {
  adbcpp::testing::MockTransport transport;

  const std::array<std::byte, 20> token{};
  const auto auth = make_message(adbcpp::protocol::kAuth,
                                adbcpp::protocol::kAuthToken, 0, token);
  transport.feed(auth.encode());
  transport.feed(token);

  const auto device = make_message(adbcpp::protocol::kCnxn, 0x01000001u, 4096u);
  transport.feed(device.encode());

  const std::array<std::byte, 4> public_key{std::byte{0x01}, std::byte{0x02},
                                           std::byte{0x03}, std::byte{0x04}};
  std::vector<std::byte> key(public_key.begin(), public_key.end());
  key.push_back(std::byte{0});

  adbcpp::Connection connection(transport, public_key);

  std::vector<std::byte> identity(
      reinterpret_cast<const std::byte *>(adbcpp::kSystemIdentity.data()),
      reinterpret_cast<const std::byte *>(adbcpp::kSystemIdentity.data()) +
          adbcpp::kSystemIdentity.size());
  identity.push_back(std::byte{0});
  const auto cnxn = make_message(adbcpp::protocol::kCnxn,
                                adbcpp::protocol::kVersion,
                                adbcpp::protocol::kMaxData, identity);
  const auto auth_out = make_message(adbcpp::protocol::kAuth,
                                    adbcpp::protocol::kAuthPublicKey, 0, key);

  std::vector<std::byte> expected;
  const auto cnxn_bytes = cnxn.encode();
  expected.insert(expected.end(), cnxn_bytes.begin(), cnxn_bytes.end());
  expected.insert(expected.end(), identity.begin(), identity.end());
  const auto auth_bytes = auth_out.encode();
  expected.insert(expected.end(), auth_bytes.begin(), auth_bytes.end());
  expected.insert(expected.end(), key.begin(), key.end());

  REQUIRE(transport.written() == expected);
  REQUIRE(connection.device_version() == 0x01000001u);
  REQUIRE(connection.max_data() == 4096u);
}

TEST_CASE("connection offers the public key when the signature is rejected",
          "[connection]") {
  adbcpp::testing::MockTransport transport;

  const std::array<std::byte, 20> token{};
  const auto auth = make_message(adbcpp::protocol::kAuth,
                                adbcpp::protocol::kAuthToken, 0, token);
  transport.feed(auth.encode());
  transport.feed(token);
  transport.feed(auth.encode());
  transport.feed(token);

  const auto device = make_message(adbcpp::protocol::kCnxn, 0x01000001u, 4096u);
  transport.feed(device.encode());

  const std::array<std::byte, 4> public_key{std::byte{0x01}, std::byte{0x02},
                                           std::byte{0x03}, std::byte{0x04}};
  std::vector<std::byte> key(public_key.begin(), public_key.end());
  key.push_back(std::byte{0});

  const std::vector<std::byte> signature(256, std::byte{0xAB});
  const adbcpp::Connection::Signer signer =
      [signature](std::span<const std::byte>) { return signature; };

  adbcpp::Connection connection(transport, public_key, signer);

  std::vector<std::byte> identity(
      reinterpret_cast<const std::byte *>(adbcpp::kSystemIdentity.data()),
      reinterpret_cast<const std::byte *>(adbcpp::kSystemIdentity.data()) +
          adbcpp::kSystemIdentity.size());
  identity.push_back(std::byte{0});
  const auto cnxn = make_message(adbcpp::protocol::kCnxn,
                                adbcpp::protocol::kVersion,
                                adbcpp::protocol::kMaxData, identity);
  const auto signed_auth = make_message(adbcpp::protocol::kAuth,
                                      adbcpp::protocol::kAuthSignature, 0,
                                      signature);
  const auto public_auth = make_message(adbcpp::protocol::kAuth,
                                      adbcpp::protocol::kAuthPublicKey, 0,
                                      key);

  std::vector<std::byte> expected;
  const auto cnxn_bytes = cnxn.encode();
  expected.insert(expected.end(), cnxn_bytes.begin(), cnxn_bytes.end());
  expected.insert(expected.end(), identity.begin(), identity.end());
  const auto signed_bytes = signed_auth.encode();
  expected.insert(expected.end(), signed_bytes.begin(), signed_bytes.end());
  expected.insert(expected.end(), signature.begin(), signature.end());
  const auto public_bytes = public_auth.encode();
  expected.insert(expected.end(), public_bytes.begin(), public_bytes.end());
  expected.insert(expected.end(), key.begin(), key.end());

  REQUIRE(transport.written() == expected);
  REQUIRE(connection.device_version() == 0x01000001u);
}

TEST_CASE("connection fails when a key is required but not provided",
          "[connection]") {
  adbcpp::testing::MockTransport transport;

  const std::array<std::byte, 20> token{};
  const auto auth = make_message(adbcpp::protocol::kAuth,
                                adbcpp::protocol::kAuthToken, 0, token);
  transport.feed(auth.encode());
  transport.feed(token);

  REQUIRE_THROWS(adbcpp::Connection(transport));
}
