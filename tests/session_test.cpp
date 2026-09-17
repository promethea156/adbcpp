#include "adbcpp/session.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <vector>

#include "adbcpp/protocol/commands.hpp"
#include "adbcpp/protocol/message.hpp"
#include "adbcpp/testing/mock_transport.hpp"

// The `Session` layer is the "transport layer" from `docs/dev/protocol.md`: it
// turns a byte channel into complete messages. These tests pin down that a header
// and its payload are written and read as separate transport operations, even when
// the bytes arrive split across reads.
using adbcpp::protocol::Message;

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
    const auto frame = session.receive();

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

    session.send(outbound);

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

    session.send(outbound, payload);

    const auto header = outbound.encode();
    const auto &written = transport.written();

    REQUIRE(written.size() == header.size() + payload.size());
    REQUIRE(std::equal(header.begin(), header.end(), written.begin()));
    REQUIRE(std::equal(payload.begin(), payload.end(), written.begin() + header.size()));
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
    const auto frame = session.receive();

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
    inbound.data_crc32 = Message::compute_crc32(payload);
    inbound.magic = Message::compute_magic(inbound.command);

    transport.feed(inbound.encode());
    transport.feed(payload);

    adbcpp::Session session(transport);
    const auto frame = session.receive();

    REQUIRE(frame.header.command == inbound.command);
    REQUIRE(frame.header.data_length == payload.size());
    REQUIRE(frame.payload == payload);
}
