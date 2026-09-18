#include "adbcpp/connection.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "adbcpp/protocol/commands.hpp"
#include "adbcpp/protocol/message.hpp"
#include "adbcpp/testing/mock_transport.hpp"

// These tests drive the CNXN/AUTH handshake from `docs/dev/protocol.md` over a
// mock transport: the CNXN banner, the AUTH type 2 signature, and the AUTH type 3
// public-key fallback (blockers 10, 11, and 16 in `04-blockers.md`).
using adbcpp::protocol::Message;

namespace
{

// Returns the value of a `Result` the test expects to succeed, failing the test
// otherwise.
template <typename T>
T unwrap(adbcpp::Result<T> result)
{
    REQUIRE(result.has_value());
    return std::move(*result);
}

adbcpp::protocol::Message make_message(std::uint32_t command, std::uint32_t arg0, std::uint32_t arg1,
                                       std::span<const std::byte> payload = {})
{
    adbcpp::protocol::Message message;
    message.command = command;
    message.arg0 = arg0;
    message.arg1 = arg1;
    message.data_length = payload.size();
    message.data_check = Message::compute_checksum(payload);
    message.magic = Message::compute_magic(command);
    return message;
}

} // namespace

TEST_CASE("connection completes the handshake without authentication", "[connection]")
{
    adbcpp::testing::MockTransport transport;

    const auto device = make_message(adbcpp::protocol::kCnxn, 0x01000001u, 4096u);
    transport.feed(device.encode());

    const auto connection = unwrap(adbcpp::Connection::connect(transport));

    REQUIRE(connection.device_version() == 0x01000001u);
    REQUIRE(connection.max_data() == 4096u);
}

TEST_CASE("connection reports the device serial from its banner", "[connection]")
{
    adbcpp::testing::MockTransport transport;

    const std::string banner = "device::ABC123::ro.product.name=test;features=shell_v2,cmd";
    const auto payload = std::span(reinterpret_cast<const std::byte *>(banner.data()), banner.size());
    const auto device = make_message(adbcpp::protocol::kCnxn, 0x01000001u, 4096u, payload);
    transport.feed(device.encode());
    transport.feed(payload);

    const auto connection = unwrap(adbcpp::Connection::connect(transport));

    REQUIRE(connection.device_serial() == "ABC123");
    REQUIRE(connection.supports_feature("shell_v2"));
}

TEST_CASE("connection reports an empty serial when the banner has none", "[connection]")
{
    adbcpp::testing::MockTransport transport;

    const std::string banner = "device::ro.product.name=test;features=shell_v2";
    const auto payload = std::span(reinterpret_cast<const std::byte *>(banner.data()), banner.size());
    const auto device = make_message(adbcpp::protocol::kCnxn, 0x01000001u, 4096u, payload);
    transport.feed(device.encode());
    transport.feed(payload);

    const auto connection = unwrap(adbcpp::Connection::connect(transport));

    REQUIRE(connection.device_serial().empty());
    REQUIRE(connection.supports_feature("shell_v2"));
}

TEST_CASE("connection prefers the transport serial over the banner", "[connection]")
{
    adbcpp::testing::MockTransport transport;
    transport.set_serial("USB123");

    const std::string banner = "device::BANNER456::features=shell_v2";
    const auto payload = std::span(reinterpret_cast<const std::byte *>(banner.data()), banner.size());
    const auto device = make_message(adbcpp::protocol::kCnxn, 0x01000001u, 4096u, payload);
    transport.feed(device.encode());
    transport.feed(payload);

    const auto connection = unwrap(adbcpp::Connection::connect(transport));

    REQUIRE(connection.device_serial() == "USB123");
}

TEST_CASE("connection answers an AUTH request with the public key", "[connection]")
{
    adbcpp::testing::MockTransport transport;

    const std::array<std::byte, 20> token{};
    const auto auth = make_message(adbcpp::protocol::kAuth, adbcpp::protocol::kAuthToken, 0, token);
    transport.feed(auth.encode());
    transport.feed(token);

    const auto device = make_message(adbcpp::protocol::kCnxn, 0x01000001u, 4096u);
    transport.feed(device.encode());

    const std::array<std::byte, 4> public_key{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
    std::vector<std::byte> key(public_key.begin(), public_key.end());
    key.push_back(std::byte{0});

    const auto connection = unwrap(adbcpp::Connection::connect(transport, public_key));

    std::vector<std::byte> identity(
        reinterpret_cast<const std::byte *>(adbcpp::kSystemIdentity.data()),
        reinterpret_cast<const std::byte *>(adbcpp::kSystemIdentity.data()) + adbcpp::kSystemIdentity.size());
    const auto cnxn =
        make_message(adbcpp::protocol::kCnxn, adbcpp::protocol::kVersion, adbcpp::protocol::kMaxData, identity);
    const auto auth_out = make_message(adbcpp::protocol::kAuth, adbcpp::protocol::kAuthPublicKey, 0, key);

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

TEST_CASE("connection offers the public key when the signature is rejected", "[connection]")
{
    adbcpp::testing::MockTransport transport;

    const std::array<std::byte, 20> token{};
    const auto auth = make_message(adbcpp::protocol::kAuth, adbcpp::protocol::kAuthToken, 0, token);
    transport.feed(auth.encode());
    transport.feed(token);
    transport.feed(auth.encode());
    transport.feed(token);

    const auto device = make_message(adbcpp::protocol::kCnxn, 0x01000001u, 4096u);
    transport.feed(device.encode());

    const std::array<std::byte, 4> public_key{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
    std::vector<std::byte> key(public_key.begin(), public_key.end());
    key.push_back(std::byte{0});

    const std::vector<std::byte> signature(256, std::byte{0xAB});
    const adbcpp::Connection::Signer signer = [signature](std::span<const std::byte>)
    {
        return signature;
    };

    const auto connection = unwrap(adbcpp::Connection::connect(transport, public_key, signer));

    std::vector<std::byte> identity(
        reinterpret_cast<const std::byte *>(adbcpp::kSystemIdentity.data()),
        reinterpret_cast<const std::byte *>(adbcpp::kSystemIdentity.data()) + adbcpp::kSystemIdentity.size());
    const auto cnxn =
        make_message(adbcpp::protocol::kCnxn, adbcpp::protocol::kVersion, adbcpp::protocol::kMaxData, identity);
    const auto signed_auth = make_message(adbcpp::protocol::kAuth, adbcpp::protocol::kAuthSignature, 0, signature);
    const auto public_auth = make_message(adbcpp::protocol::kAuth, adbcpp::protocol::kAuthPublicKey, 0, key);

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

TEST_CASE("connection fails when a key is required but not provided", "[connection]")
{
    adbcpp::testing::MockTransport transport;

    const std::array<std::byte, 20> token{};
    const auto auth = make_message(adbcpp::protocol::kAuth, adbcpp::protocol::kAuthToken, 0, token);
    transport.feed(auth.encode());
    transport.feed(token);

    const auto connection = adbcpp::Connection::connect(transport);
    REQUIRE_FALSE(connection.has_value());
    REQUIRE(connection.error().code == adbcpp::ErrorCode::Crypto);
}
