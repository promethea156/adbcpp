#include "adbcpp/stream.hpp"

#include <stdexcept>

#include "adbcpp/protocol/commands.hpp"

namespace adbcpp {
namespace {

protocol::Message make_message(std::uint32_t command, std::uint32_t arg0,
                                std::uint32_t arg1,
                                std::span<const std::byte> payload) {
  protocol::Message message;
  message.command = command;
  message.arg0 = arg0;
  message.arg1 = arg1;
  message.data_length = payload.size();
  message.data_crc32 = protocol::Message::compute_crc32(payload);
  message.magic = protocol::Message::compute_magic(command);
  return message;
}

} // namespace

Stream::Stream(Connection &connection, std::string_view service)
    : connection_(&connection), service_(service),
      local_id_(connection.allocate_local_id()) {
  std::string destination(service_);
  destination.push_back('\0');
  const auto payload = std::span(
      reinterpret_cast<const std::byte *>(destination.data()), destination.size());
  const std::uint32_t send_buffer =
      connection_->supports_delayed_ack()
          ? protocol::kInitialDelayedAckBytes
          : 0;
  connection_->send(make_message(protocol::kOpen, local_id_, send_buffer,
                                payload));

  const auto frame = connection_->receive();
  if (frame.header.command != protocol::kOkay) {
    throw std::runtime_error("adbcpp: failed to open the stream");
  }
  remote_id_ = frame.header.arg0;
}

Stream::~Stream() {
  if (!closed_) {
    try {
      connection_->send(make_message(protocol::kClse, local_id_, remote_id_, {}));
    } catch (...) {
      // Closing is best effort during destruction.
    }
  }
}

void Stream::write(std::span<const std::byte> data) {
  connection_->send(make_message(protocol::kWrte, local_id_, remote_id_, data),
                    data);
}

std::vector<std::byte> Stream::read_all() {
  std::vector<std::byte> output;
  while (!closed_) {
    auto frame = connection_->receive();
    switch (frame.header.command) {
    case protocol::kWrte:
      output.insert(output.end(), frame.payload.begin(), frame.payload.end());
      connection_->send(make_message(protocol::kOkay, local_id_, remote_id_, {}));
      break;
    case protocol::kClse:
      closed_ = true;
      connection_->send(make_message(protocol::kClse, local_id_, remote_id_, {}));
      break;
    default:
      throw std::runtime_error("adbcpp: unexpected message on the stream");
    }
  }
  return output;
}

} // namespace adbcpp
