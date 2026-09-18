#include "adbcpp/shell.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "adbcpp/connection.hpp"
#include "adbcpp/protocol/commands.hpp"
#include "adbcpp/protocol/message.hpp"
#include "adbcpp/testing/mock_transport.hpp"

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
    message.data_length = *adbcpp::protocol::Message::data_length_of(payload);
    message.data_check = adbcpp::protocol::Message::compute_checksum(payload);
    message.magic = adbcpp::protocol::Message::compute_magic(command);
    return message;
}

void feed_frame(adbcpp::testing::MockTransport &transport, const adbcpp::protocol::Message &header,
                std::span<const std::byte> payload = {})
{
    transport.feed(header.encode());
    if (!payload.empty())
    {
        transport.feed(payload);
    }
}

// Feeds the device's CNXN and the OKAY that accepts the stream's OPEN. The
// device's id for the stream is 7, ours is 2.
void feed_device(adbcpp::testing::MockTransport &transport)
{
    const std::string banner = "device::features=shell_v2";
    const auto banner_bytes = std::span(reinterpret_cast<const std::byte *>(banner.data()), banner.size());
    feed_frame(transport, make_message(adbcpp::protocol::kCnxn, 0x01000001u, 4096u, banner_bytes), banner_bytes);
    feed_frame(transport, make_message(adbcpp::protocol::kOkay, 7, 2));
}

// Feeds one shell_v2 packet inside a WRTE: a one-byte id, a four-byte
// little-endian length, and the data.
void feed_shell(adbcpp::testing::MockTransport &transport, std::uint8_t id, std::span<const std::byte> data)
{
    std::vector<std::byte> packet(5 + data.size());
    packet[0] = static_cast<std::byte>(id);
    packet[1] = static_cast<std::byte>(data.size() & 0xFFu);
    packet[2] = static_cast<std::byte>((data.size() >> 8) & 0xFFu);
    packet[3] = static_cast<std::byte>((data.size() >> 16) & 0xFFu);
    packet[4] = static_cast<std::byte>((data.size() >> 24) & 0xFFu);
    std::copy(data.begin(), data.end(), packet.begin() + 5);
    feed_frame(transport, make_message(adbcpp::protocol::kWrte, 7, 2, packet), packet);
}

std::span<const std::byte> bytes_of(const std::string &text)
{
    return std::span(reinterpret_cast<const std::byte *>(text.data()), text.size());
}

// The exit packet's data is a single byte holding the exit status, and its length
// is always 1. adbd writes it with `data()[0] = exit_code; Write(kIdExit, 1)`.
void feed_exit(adbcpp::testing::MockTransport &transport, std::uint8_t exit_code)
{
    const std::array<std::byte, 1> data{static_cast<std::byte>(exit_code)};
    feed_shell(transport, 3, data);
}

void feed_close(adbcpp::testing::MockTransport &transport)
{
    feed_frame(transport, make_message(adbcpp::protocol::kClse, 7, 2));
}

} // namespace

TEST_CASE("run reassembles stdout and reads the exit code", "[shell]")
{
    adbcpp::testing::MockTransport transport;
    feed_device(transport);
    feed_shell(transport, 1, bytes_of("hello\n"));
    feed_exit(transport, 0);
    feed_close(transport);

    auto connection = unwrap(adbcpp::Connection::connect(transport));
    const auto result = unwrap(adbcpp::run(connection, "echo hello"));

    REQUIRE(result.output == "hello\n");
    REQUIRE(result.exit_code == 0);
}

TEST_CASE("run merges stderr into the output", "[shell]")
{
    adbcpp::testing::MockTransport transport;
    feed_device(transport);
    feed_shell(transport, 1, bytes_of("out"));
    feed_shell(transport, 2, bytes_of("err"));
    feed_exit(transport, 0);
    feed_close(transport);

    auto connection = unwrap(adbcpp::Connection::connect(transport));
    const auto result = unwrap(adbcpp::run(connection, "command"));

    REQUIRE(result.output == "outerr");
}

TEST_CASE("run reads a nonzero exit code from the exit packet's data", "[shell]")
{
    adbcpp::testing::MockTransport transport;
    feed_device(transport);
    feed_exit(transport, 42);
    feed_close(transport);

    auto connection = unwrap(adbcpp::Connection::connect(transport));
    const auto result = unwrap(adbcpp::run(connection, "false"));

    REQUIRE(result.exit_code == 42);
}
