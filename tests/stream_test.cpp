#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "adbcpp/connection.hpp"
#include "adbcpp/protocol/commands.hpp"
#include "adbcpp/protocol/message.hpp"
#include "adbcpp/stream.hpp"
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

void feed_frame(adbcpp::testing::MockTransport &transport,
                const adbcpp::protocol::Message &header,
                std::span<const std::byte> payload = {}) {
  transport.feed(header.encode());
  if (!payload.empty()) {
    transport.feed(payload);
  }
}

constexpr std::uint32_t kLocalId = 1;
constexpr std::uint32_t kRemoteId = 7;

} // namespace

TEST_CASE("stream opens and collects output until close", "[stream]") {
  adbcpp::testing::MockTransport transport;

  const auto device = make_message(adbcpp::protocol::kCnxn, 0x01000001u, 4096u);
  feed_frame(transport, device);
  feed_frame(transport, make_message(adbcpp::protocol::kOkay, kRemoteId,
                                    kLocalId));

  const std::string output = "hello\n";
  const auto output_bytes = std::span(
      reinterpret_cast<const std::byte *>(output.data()), output.size());
  feed_frame(transport, make_message(adbcpp::protocol::kWrte, kLocalId,
                                    kRemoteId, output_bytes), output_bytes);
  feed_frame(transport, make_message(adbcpp::protocol::kClse, kLocalId,
                                    kRemoteId));

  adbcpp::Connection connection(transport);
  adbcpp::Stream stream(connection, "shell:echo hello");

  REQUIRE(stream.service() == "shell:echo hello");
  const auto collected = stream.read_all();

  REQUIRE(collected.size() == output.size());
  REQUIRE(std::equal(collected.begin(), collected.end(), output_bytes.begin()));
}

TEST_CASE("stream sends OKAY for each WRTE", "[stream]") {
  adbcpp::testing::MockTransport transport;

  const auto device = make_message(adbcpp::protocol::kCnxn, 0x01000001u, 4096u);
  feed_frame(transport, device);
  feed_frame(transport, make_message(adbcpp::protocol::kOkay, kRemoteId,
                                    kLocalId));
  feed_frame(transport, make_message(adbcpp::protocol::kWrte, kLocalId,
                                    kRemoteId));
  feed_frame(transport, make_message(adbcpp::protocol::kClse, kLocalId,
                                    kRemoteId));

  adbcpp::Connection connection(transport);
  adbcpp::Stream stream(connection, "shell:id");
  const auto before = transport.written().size();
  stream.read_all();

  const auto expected =
      make_message(adbcpp::protocol::kOkay, kLocalId, kRemoteId, {}).encode();
  const auto &written = transport.written();

  REQUIRE(written.size() >= before + expected.size());
  REQUIRE(std::equal(written.begin() + static_cast<std::ptrdiff_t>(before),
                     written.begin() + static_cast<std::ptrdiff_t>(before) +
                         static_cast<std::ptrdiff_t>(expected.size()),
                     expected.begin()));
}
