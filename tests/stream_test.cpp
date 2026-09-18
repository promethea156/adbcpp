#include "adbcpp/stream.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "adbcpp/connection.hpp"
#include "adbcpp/protocol/commands.hpp"
#include "adbcpp/protocol/message.hpp"
#include "adbcpp/testing/mock_transport.hpp"

// These tests pin down the OPEN/OKAY/WRTE/CLSE stream lifecycle from
// `docs/dev/protocol.md`, including the delayed-acknowledgement window in OPEN's
// `arg1` (see `docs/dev/delayed_ack.md` and blocker 12 in `04-blockers.md`).
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

void feed_frame(adbcpp::testing::MockTransport &transport, const adbcpp::protocol::Message &header,
                std::span<const std::byte> payload = {})
{
    transport.feed(header.encode());
    if (!payload.empty())
    {
        transport.feed(payload);
    }
}

constexpr std::uint32_t kLocalId = 2;
constexpr std::uint32_t kRemoteId = 7;

} // namespace

TEST_CASE("stream opens with a zero send buffer when delayed ack is off", "[stream]")
{
    adbcpp::testing::MockTransport transport;

    const auto device = make_message(adbcpp::protocol::kCnxn, 0x01000001u, 4096u);
    feed_frame(transport, device);

    auto connection = unwrap(adbcpp::Connection::connect(transport));
    REQUIRE_FALSE(connection.supports_delayed_ack());
    const auto before = transport.written().size();

    feed_frame(transport, make_message(adbcpp::protocol::kOkay, kRemoteId, kLocalId));

    // Without a negotiated delayed ack the OPEN window (`arg1`) must be zero,
    // otherwise a peer that does not implement the feature closes the stream.
    const std::string service = "shell,v2,raw:echo hello";
    std::string destination(service);
    destination.push_back('\0');
    const auto payload = std::span(reinterpret_cast<const std::byte *>(destination.data()), destination.size());
    const auto open = make_message(adbcpp::protocol::kOpen, kLocalId, 0, payload);

    std::vector<std::byte> expected;
    const auto open_bytes = open.encode();
    expected.insert(expected.end(), open_bytes.begin(), open_bytes.end());
    expected.insert(expected.end(), payload.begin(), payload.end());

    {
        auto stream = unwrap(adbcpp::Stream::open(connection, service));
        const auto &written = transport.written();
        REQUIRE(written.size() == before + expected.size());
        REQUIRE(std::equal(written.begin() + static_cast<std::ptrdiff_t>(before), written.end(), expected.begin()));
    }
}

TEST_CASE("stream sends the delayed ack window when negotiated", "[stream]")
{
    adbcpp::testing::MockTransport transport;

    const std::string banner = "device::features=shell_v2,delayed_ack";
    const auto banner_bytes = std::span(reinterpret_cast<const std::byte *>(banner.data()), banner.size());
    feed_frame(transport, make_message(adbcpp::protocol::kCnxn, 0x01000001u, 4096u, banner_bytes), banner_bytes);

    auto connection = unwrap(adbcpp::Connection::connect(transport, {}, {}, true));
    REQUIRE(connection.supports_delayed_ack());
    const auto before = transport.written().size();

    feed_frame(transport, make_message(adbcpp::protocol::kOkay, kRemoteId, kLocalId));

    const std::string service = "shell,v2,raw:echo hello";
    std::string destination(service);
    destination.push_back('\0');
    const auto payload = std::span(reinterpret_cast<const std::byte *>(destination.data()), destination.size());
    const auto open =
        make_message(adbcpp::protocol::kOpen, kLocalId, adbcpp::protocol::kInitialDelayedAckBytes, payload);

    std::vector<std::byte> expected;
    const auto open_bytes = open.encode();
    expected.insert(expected.end(), open_bytes.begin(), open_bytes.end());
    expected.insert(expected.end(), payload.begin(), payload.end());

    {
        auto stream = unwrap(adbcpp::Stream::open(connection, service));
        const auto &written = transport.written();
        REQUIRE(written.size() == before + expected.size());
        REQUIRE(std::equal(written.begin() + static_cast<std::ptrdiff_t>(before), written.end(), expected.begin()));
    }
}

TEST_CASE("stream opens and collects output until close", "[stream]")
{
    adbcpp::testing::MockTransport transport;

    const auto device = make_message(adbcpp::protocol::kCnxn, 0x01000001u, 4096u);
    feed_frame(transport, device);
    feed_frame(transport, make_message(adbcpp::protocol::kOkay, kRemoteId, kLocalId));

    const std::string output = "hello\n";
    const auto output_bytes = std::span(reinterpret_cast<const std::byte *>(output.data()), output.size());
    feed_frame(transport, make_message(adbcpp::protocol::kWrte, kRemoteId, kLocalId, output_bytes), output_bytes);
    feed_frame(transport, make_message(adbcpp::protocol::kClse, kRemoteId, kLocalId));

    auto connection = unwrap(adbcpp::Connection::connect(transport));
    auto stream = unwrap(adbcpp::Stream::open(connection, "shell:echo hello"));

    REQUIRE(stream.service() == "shell:echo hello");
    const auto collected = unwrap(stream.read_all());

    REQUIRE(collected.size() == output.size());
    REQUIRE(std::equal(collected.begin(), collected.end(), output_bytes.begin()));
}

TEST_CASE("stream sends OKAY for each WRTE", "[stream]")
{
    adbcpp::testing::MockTransport transport;

    const auto device = make_message(adbcpp::protocol::kCnxn, 0x01000001u, 4096u);
    feed_frame(transport, device);
    feed_frame(transport, make_message(adbcpp::protocol::kOkay, kRemoteId, kLocalId));
    feed_frame(transport, make_message(adbcpp::protocol::kWrte, kRemoteId, kLocalId));
    feed_frame(transport, make_message(adbcpp::protocol::kClse, kRemoteId, kLocalId));

    auto connection = unwrap(adbcpp::Connection::connect(transport));
    auto stream = unwrap(adbcpp::Stream::open(connection, "shell:id"));
    const auto before = transport.written().size();
    REQUIRE(stream.read_all().has_value());

    const auto expected = make_message(adbcpp::protocol::kOkay, kLocalId, kRemoteId, {}).encode();
    const auto &written = transport.written();

    REQUIRE(written.size() >= before + expected.size());
    REQUIRE(
        std::equal(written.begin() + static_cast<std::ptrdiff_t>(before),
                   written.begin() + static_cast<std::ptrdiff_t>(before) + static_cast<std::ptrdiff_t>(expected.size()),
                   expected.begin()));
}

TEST_CASE("stream rejects a write larger than the negotiated maximum payload", "[stream]")
{
    adbcpp::testing::MockTransport transport;

    // The device advertises a 4096-byte maximum payload.
    const auto device = make_message(adbcpp::protocol::kCnxn, 0x01000001u, 4096u);
    feed_frame(transport, device);
    feed_frame(transport, make_message(adbcpp::protocol::kOkay, kRemoteId, kLocalId));

    auto connection = unwrap(adbcpp::Connection::connect(transport));
    auto stream = unwrap(adbcpp::Stream::open(connection, "shell:id"));

    const std::vector<std::byte> too_large(4097, std::byte{0});
    const auto written = stream.write(too_large);
    REQUIRE_FALSE(written.has_value());
    REQUIRE(written.error().code == adbcpp::ErrorCode::InvalidArgument);
}

TEST_CASE("stream ignores a frame for another stream while opening", "[stream]")
{
    adbcpp::testing::MockTransport transport;

    const auto device = make_message(adbcpp::protocol::kCnxn, 0x01000001u, 4096u);
    feed_frame(transport, device);

    // A stray CLOSE for another stream arrives before this stream's OKAY. The ids
    // are relative to the sender, so `arg1` is our local id.
    feed_frame(transport, make_message(adbcpp::protocol::kClse, kRemoteId, 9));
    feed_frame(transport, make_message(adbcpp::protocol::kOkay, kRemoteId, kLocalId));

    auto connection = unwrap(adbcpp::Connection::connect(transport));
    auto stream = unwrap(adbcpp::Stream::open(connection, "shell:id"));

    REQUIRE(stream.service() == "shell:id");
}

TEST_CASE("stream skips the OKAY that acknowledges a write", "[stream]")
{
    adbcpp::testing::MockTransport transport;

    const auto device = make_message(adbcpp::protocol::kCnxn, 0x01000001u, 4096u);
    feed_frame(transport, device);
    feed_frame(transport, make_message(adbcpp::protocol::kOkay, kRemoteId, kLocalId));

    // The device acknowledges the write we send, then sends its own WRTE. The
    // acknowledgement carries no data, so the read must skip it.
    const std::string output = "hello\n";
    const auto output_bytes = std::span(reinterpret_cast<const std::byte *>(output.data()), output.size());
    feed_frame(transport, make_message(adbcpp::protocol::kOkay, kRemoteId, kLocalId));
    feed_frame(transport, make_message(adbcpp::protocol::kWrte, kRemoteId, kLocalId, output_bytes), output_bytes);
    feed_frame(transport, make_message(adbcpp::protocol::kClse, kRemoteId, kLocalId));

    auto connection = unwrap(adbcpp::Connection::connect(transport));
    auto stream = unwrap(adbcpp::Stream::open(connection, "sync:"));

    const std::string request = "LIST";
    const auto request_bytes = std::span(reinterpret_cast<const std::byte *>(request.data()), request.size());
    REQUIRE(stream.write(request_bytes).has_value());

    const auto collected = unwrap(stream.read_all());
    REQUIRE(collected.size() == output.size());
    REQUIRE(std::equal(collected.begin(), collected.end(), output_bytes.begin()));
}
