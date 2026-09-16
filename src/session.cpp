#include "adbcpp/session.hpp"

#include <array>
#include <cstddef>
#include <stdexcept>

namespace adbcpp {
namespace {

void read_exact(Transport &transport, std::span<std::byte> buffer) {
  std::size_t total = 0;
  while (total < buffer.size()) {
    const std::size_t count = transport.read(buffer.subspan(total));
    if (count == 0) {
      throw std::runtime_error("adbcpp: unexpected end of stream");
    }
    total += count;
  }
}

} // namespace

Session::Session(Transport &transport) noexcept : transport_(&transport) {}

void Session::send(const protocol::Message &header,
                   std::span<const std::byte> payload) {
  const auto bytes = header.encode();
  transport_->write(bytes);
  if (!payload.empty()) {
    transport_->write(payload);
  }
}

Frame Session::receive() {
  std::array<std::byte, protocol::kMessageHeaderSize> bytes{};
  read_exact(*transport_, bytes);

  Frame frame;
  frame.header = protocol::Message::decode(bytes);
  frame.payload.resize(frame.header.data_length);
  if (!frame.payload.empty()) {
    read_exact(*transport_, frame.payload);
  }
  return frame;
}

} // namespace adbcpp
