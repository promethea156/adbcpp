#include "adbcpp/connection.hpp"

#include <stdexcept>
#include <string>
#include <vector>

#include "adbcpp/protocol/commands.hpp"

namespace adbcpp {

Connection::Connection(Transport &transport,
                        std::span<const std::byte> public_key)
    : session_(transport) {
  const auto identity = std::span(
      reinterpret_cast<const std::byte *>(kSystemIdentity.data()),
      kSystemIdentity.size());

  protocol::Message connect;
  connect.command = protocol::kCnxn;
  connect.arg0 = protocol::kVersion;
  connect.arg1 = protocol::kMaxData;
  connect.data_length = identity.size();
  connect.data_crc32 = protocol::Message::compute_crc32(identity);
  connect.magic = protocol::Message::compute_magic(connect.command);
  session_.send(connect, identity);

  auto frame = session_.receive();
  if (frame.header.command == protocol::kAuth) {
    if (public_key.empty()) {
      throw std::runtime_error(
          "adbcpp: device requires authentication but no public key was given");
    }
    std::vector<std::byte> key(public_key.begin(), public_key.end());
    key.push_back(std::byte{0});

    protocol::Message auth;
    auth.command = protocol::kAuth;
    auth.arg0 = protocol::kAuthPublicKey;
    auth.data_length = key.size();
    auth.data_crc32 = protocol::Message::compute_crc32(key);
    auth.magic = protocol::Message::compute_magic(auth.command);
    session_.send(auth, key);
    frame = session_.receive();
  }

  if (frame.header.command != protocol::kCnxn) {
    throw std::runtime_error("adbcpp: unexpected response to the CNXN message");
  }

  device_version_ = frame.header.arg0;
  max_data_ = frame.header.arg1;

  const std::string banner(
      reinterpret_cast<const char *>(frame.payload.data()),
      frame.payload.size());
  delayed_ack_ = banner.find("delayed_ack") != std::string::npos;
}

void Connection::send(const protocol::Message &header,
                      std::span<const std::byte> payload) {
  session_.send(header, payload);
}

Frame Connection::receive() { return session_.receive(); }

} // namespace adbcpp
