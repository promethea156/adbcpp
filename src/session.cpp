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

void Session::send(const protocol::Message &message) {
  const auto bytes = message.encode();
  transport_->write(bytes);
}

protocol::Message Session::receive() {
  std::array<std::byte, protocol::kMessageHeaderSize> bytes{};
  read_exact(*transport_, bytes);
  return protocol::Message::decode(bytes);
}

} // namespace adbcpp
