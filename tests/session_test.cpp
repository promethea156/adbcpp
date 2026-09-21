#include "adbcpp/session.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <utility>
#include <vector>

#include "adbcpp/protocol/commands.hpp"
#include "adbcpp/protocol/message.hpp"
#include "adbcpp/testing/mock_transport.hpp"

// The `Session` layer is the "transport layer" from `docs/dev/protocol.md`: it
// turns a byte channel into complete messages. These tests pin down that a header
// and its payload are written and read as separate transport operations, even when
// the bytes arrive split across reads.
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

} // namespace

TEST_CASE("session receives a framed message from the transport", "[session]")
{
    adbcpp::testing::MockTransport transport;

    Message inbound;
    inbound.command = adbcpp::protocol::kCnxn;
    inbound.arg0 = 0x01000001u;
    inbound.arg1 = 4096u;
    inbound.magic = Message::compute_magic(inbound.command);
    transport.feed(inbound.encode());

    adbcpp::Session session(transport);
    const auto frame = unwrap(session.receive());

    REQUIRE(frame.header.command == inbound.command);
    REQUIRE(frame.header.arg0 == inbound.arg0);
    REQUIRE(frame.header.arg1 == inbound.arg1);
    REQUIRE(frame.header.magic == inbound.magic);
    REQUIRE(frame.payload.empty());
}

TEST_CASE("session writes a framed message to the transport", "[session]")
{
    adbcpp::testing::MockTransport transport;
    adbcpp::Session session(transport);

    Message outbound;
    outbound.command = adbcpp::protocol::kOkay;
    outbound.magic = Message::compute_magic(outbound.command);

    REQUIRE(session.send(outbound).has_value());

    const auto expected = outbound.encode();
    const auto &written = transport.written();

    REQUIRE(written.size() == expected.size());
    REQUIRE(std::equal(written.begin(), written.end(), expected.begin()));
}

TEST_CASE("session writes a payload as a separate transport write", "[session]")
{
    adbcpp::testing::MockTransport transport;
    adbcpp::Session session(transport);

    const std::array<std::byte, 5> payload{std::byte{'h'}, std::byte{'e'}, std::byte{'l'}, std::byte{'l'},
                                           std::byte{'o'}};

    Message outbound;
    outbound.command = adbcpp::protocol::kWrte;
    outbound.data_length = payload.size();
    outbound.magic = Message::compute_magic(outbound.command);

    REQUIRE(session.send(outbound, payload).has_value());

    const auto header = outbound.encode();
    const auto &written = transport.written();

    REQUIRE(written.size() == header.size() + payload.size());
    REQUIRE(std::equal(header.begin(), header.end(), written.begin()));
    REQUIRE(std::equal(payload.begin(), payload.end(), written.begin() + header.size()));
}

TEST_CASE("session rejects a header whose data_length does not match the payload", "[session]")
{
    adbcpp::testing::MockTransport transport;
    adbcpp::Session session(transport);

    const std::array<std::byte, 5> payload{std::byte{'h'}, std::byte{'e'}, std::byte{'l'}, std::byte{'l'},
                                           std::byte{'o'}};

    Message outbound;
    outbound.command = adbcpp::protocol::kWrte;
    // The header advertises one byte more than the payload, which is the shape
    // blocker 13 had: the device would wait for a byte that never arrives.
    outbound.data_length = payload.size() + 1;
    outbound.magic = Message::compute_magic(outbound.command);

    const auto sent = session.send(outbound, payload);
    REQUIRE_FALSE(sent.has_value());
    REQUIRE(sent.error().code == adbcpp::ErrorCode::InvalidArgument);

    // Nothing was written, so the stream is not desynchronized.
    REQUIRE(transport.written().empty());
}

TEST_CASE("session reassembles a message split across reads", "[session]")
{
    adbcpp::testing::MockTransport transport;

    Message inbound;
    inbound.command = adbcpp::protocol::kWrte;
    inbound.arg0 = 1u;
    inbound.arg1 = 2u;
    inbound.magic = Message::compute_magic(inbound.command);

    // Split the header itself in two to prove `read_exact` loops until all 24
    // bytes have arrived, as a USB transfer or TCP read may return only part.
    const auto encoded = inbound.encode();
    transport.feed(std::span(encoded).first(5));
    transport.feed(std::span(encoded).subspan(5));

    adbcpp::Session session(transport);
    const auto frame = unwrap(session.receive());

    REQUIRE(frame.header.command == inbound.command);
    REQUIRE(frame.header.arg0 == inbound.arg0);
    REQUIRE(frame.header.arg1 == inbound.arg1);
}

TEST_CASE("session reads a payload after its header", "[session]")
{
    adbcpp::testing::MockTransport transport;

    const std::vector<std::byte> payload{std::byte{'w'}, std::byte{'o'}, std::byte{'r'}, std::byte{'l'},
                                         std::byte{'d'}};

    Message inbound;
    inbound.command = adbcpp::protocol::kWrte;
    inbound.data_length = payload.size();
    inbound.data_check = Message::compute_checksum(payload);
    inbound.magic = Message::compute_magic(inbound.command);

    transport.feed(inbound.encode());
    transport.feed(payload);

    adbcpp::Session session(transport);
    const auto frame = unwrap(session.receive());

    REQUIRE(frame.header.command == inbound.command);
    REQUIRE(frame.header.data_length == payload.size());
    REQUIRE(frame.payload == payload);
}

TEST_CASE("session accepts a header with no CRC", "[session]")
{
    adbcpp::testing::MockTransport transport;

    const std::vector<std::byte> payload{std::byte{'w'}, std::byte{'o'}, std::byte{'r'}, std::byte{'l'},
                                         std::byte{'d'}};

    Message inbound;
    inbound.command = adbcpp::protocol::kWrte;
    inbound.data_length = payload.size();
    // Protocol 0x01000001 and later leave the CRC at zero, which means "not set"
    // rather than "a CRC of zero".
    inbound.data_check = 0;
    inbound.magic = Message::compute_magic(inbound.command);

    transport.feed(inbound.encode());
    transport.feed(payload);

    adbcpp::Session session(transport);
    const auto frame = unwrap(session.receive());

    REQUIRE(frame.payload == payload);
    REQUIRE_FALSE(transport.closed());
}

TEST_CASE("session rejects a header whose magic does not match its command", "[session]")
{
    adbcpp::testing::MockTransport transport;

    Message inbound;
    inbound.command = adbcpp::protocol::kCnxn;
    inbound.magic = Message::compute_magic(adbcpp::protocol::kOkay);
    transport.feed(inbound.encode());

    adbcpp::Session session(transport);
    const auto frame = session.receive();

    REQUIRE_FALSE(frame.has_value());
    REQUIRE(frame.error().code == adbcpp::ErrorCode::Protocol);
    // A framing error cannot be recovered from, so the transport is closed.
    REQUIRE(transport.closed());
}

TEST_CASE("session rejects a payload longer than the maximum", "[session]")
{
    adbcpp::testing::MockTransport transport;

    Message inbound;
    inbound.command = adbcpp::protocol::kWrte;
    inbound.data_length = adbcpp::protocol::kMaxData + 1;
    inbound.magic = Message::compute_magic(inbound.command);
    transport.feed(inbound.encode());

    adbcpp::Session session(transport);
    const auto frame = session.receive();

    REQUIRE_FALSE(frame.has_value());
    REQUIRE(frame.error().code == adbcpp::ErrorCode::Protocol);
    REQUIRE(transport.closed());
}

TEST_CASE("session rejects a payload whose CRC does not match", "[session]")
{
    adbcpp::testing::MockTransport transport;

    const std::vector<std::byte> payload{std::byte{'w'}, std::byte{'o'}, std::byte{'r'}, std::byte{'l'},
                                         std::byte{'d'}};

    Message inbound;
    inbound.command = adbcpp::protocol::kWrte;
    inbound.data_length = payload.size();
    inbound.data_check = Message::compute_checksum(payload) ^ 1u;
    inbound.magic = Message::compute_magic(inbound.command);

    transport.feed(inbound.encode());
    transport.feed(payload);

    adbcpp::Session session(transport);
    const auto frame = session.receive();

    REQUIRE_FALSE(frame.has_value());
    REQUIRE(frame.error().code == adbcpp::ErrorCode::Protocol);
    REQUIRE(transport.closed());
}

TEST_CASE("session close closes the transport and marks the session closed", "[session]")
{
    adbcpp::testing::MockTransport transport;
    adbcpp::Session session(transport);
    REQUIRE(session.is_open());

    session.close();
    REQUIRE_FALSE(session.is_open());
    REQUIRE(transport.closed());

    // Closing twice is harmless.
    session.close();

    Message outbound;
    outbound.command = adbcpp::protocol::kOkay;
    outbound.magic = Message::compute_magic(outbound.command);
    const auto sent = session.send(outbound);
    REQUIRE_FALSE(sent.has_value());
    REQUIRE(sent.error().code == adbcpp::ErrorCode::Transport);

    const auto frame = session.receive();
    REQUIRE_FALSE(frame.has_value());
    REQUIRE(frame.error().code == adbcpp::ErrorCode::Transport);
}

TEST_CASE("session reports a closed session after a framing error", "[session]")
{
    adbcpp::testing::MockTransport transport;

    Message inbound;
    inbound.command = adbcpp::protocol::kCnxn;
    inbound.magic = Message::compute_magic(adbcpp::protocol::kOkay);
    transport.feed(inbound.encode());

    adbcpp::Session session(transport);
    REQUIRE_FALSE(session.receive().has_value());
    REQUIRE_FALSE(session.is_open());

    Message outbound;
    outbound.command = adbcpp::protocol::kOkay;
    outbound.magic = Message::compute_magic(outbound.command);
    const auto sent = session.send(outbound);
    REQUIRE_FALSE(sent.has_value());
    REQUIRE(sent.error().code == adbcpp::ErrorCode::Transport);
}

TEST_CASE("moving a session leaves the moved-from one not owning the transport", "[session]")
{
    adbcpp::testing::MockTransport transport;
    adbcpp::Session session(transport);

    adbcpp::Session moved(std::move(session));
    REQUIRE_FALSE(session.is_open());
    REQUIRE(moved.is_open());

    // The moved-from session is closed, so closing it must not close the
    // transport the moved-to session still uses.
    session.close();
    REQUIRE_FALSE(transport.closed());
    REQUIRE(moved.is_open());

    moved.close();
    REQUIRE(transport.closed());
}
