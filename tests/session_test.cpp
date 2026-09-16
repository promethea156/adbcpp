#include <algorithm>
#include <cstddef>

#include <catch2/catch_test_macros.hpp>

#include "adbcpp/protocol/commands.hpp"
#include "adbcpp/protocol/message.hpp"
#include "adbcpp/session.hpp"
#include "adbcpp/testing/mock_transport.hpp"

using adbcpp::protocol::Message;

TEST_CASE("session receives a framed message from the transport", "[session]") {
  adbcpp::testing::MockTransport transport;

  Message inbound;
  inbound.command = adbcpp::protocol::kCnxn;
  inbound.arg0 = 0x01000001u;
  inbound.arg1 = 4096u;
  inbound.magic = Message::compute_magic(inbound.command);
  transport.feed(inbound.encode());

  adbcpp::Session session(transport);
  const auto received = session.receive();

  REQUIRE(received.command == inbound.command);
  REQUIRE(received.arg0 == inbound.arg0);
  REQUIRE(received.arg1 == inbound.arg1);
  REQUIRE(received.magic == inbound.magic);
}

TEST_CASE("session writes a framed message to the transport", "[session]") {
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

TEST_CASE("session reassembles a message split across reads", "[session]") {
  adbcpp::testing::MockTransport transport;

  Message inbound;
  inbound.command = adbcpp::protocol::kWrte;
  inbound.arg0 = 1u;
  inbound.arg1 = 2u;
  inbound.magic = Message::compute_magic(inbound.command);

  const auto encoded = inbound.encode();
  transport.feed(std::span(encoded).first(5));
  transport.feed(std::span(encoded).subspan(5));

  adbcpp::Session session(transport);
  const auto received = session.receive();

  REQUIRE(received.command == inbound.command);
  REQUIRE(received.arg0 == inbound.arg0);
  REQUIRE(received.arg1 == inbound.arg1);
}
