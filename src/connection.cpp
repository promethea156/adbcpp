#include "adbcpp/connection.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "adbcpp/protocol/commands.hpp"

namespace adbcpp {
namespace {

protocol::Message make_auth(std::uint32_t type,
                           std::span<const std::byte> payload) {
  protocol::Message auth;
  auth.command = protocol::kAuth;
  auth.arg0 = type;
  auth.data_length = payload.size();
  auth.data_crc32 = protocol::Message::compute_crc32(payload);
  auth.magic = protocol::Message::compute_magic(auth.command);
  return auth;
}

} // namespace

Connection::Connection(Transport &transport,
                        std::span<const std::byte> public_key, Signer signer,
                        bool advertise_delayed_ack)
    : session_(transport) {
  std::string identity(kSystemIdentity);
  if (advertise_delayed_ack) {
    identity += ',';
    identity += kDelayedAckFeature;
  }
  const auto identity_bytes = std::span(
      reinterpret_cast<const std::byte *>(identity.data()), identity.size());

  protocol::Message connect;
  connect.command = protocol::kCnxn;
  connect.arg0 = protocol::kVersion;
  connect.arg1 = protocol::kMaxData;
  connect.data_length = identity_bytes.size();
  connect.data_crc32 = protocol::Message::compute_crc32(identity_bytes);
  connect.magic = protocol::Message::compute_magic(connect.command);
  session_.send(connect, identity_bytes);

  auto frame = session_.receive();
  if (frame.header.command == protocol::kAuth) {
    if (signer) {
      const auto signature = signer(frame.payload);
      session_.send(make_auth(protocol::kAuthSignature, signature), signature);
      frame = session_.receive();
    }

    // The device did not recognize the signature; offer the public key so it can
    // ask the user to authorize it, exactly like adb.
    if (frame.header.command == protocol::kAuth) {
      if (public_key.empty()) {
        throw std::runtime_error(
            "adbcpp: the device rejected the signature and no public key is "
            "available to request authorization");
      }
      std::vector<std::byte> key(public_key.begin(), public_key.end());
      key.push_back(std::byte{0});
      session_.send(make_auth(protocol::kAuthPublicKey, key), key);
      frame = session_.receive();
    }
  }

  if (frame.header.command != protocol::kCnxn) {
    throw std::runtime_error("adbcpp: unexpected response to the CNXN message");
  }

  device_version_ = frame.header.arg0;
  max_data_ = frame.header.arg1;

  const std::string banner(
      reinterpret_cast<const char *>(frame.payload.data()),
      frame.payload.size());
  delayed_ack_ = advertise_delayed_ack &&
                 banner.find(kDelayedAckFeature) != std::string::npos;
}

void Connection::send(const protocol::Message &header,
                      std::span<const std::byte> payload) {
  session_.send(header, payload);
}

Frame Connection::receive() { return session_.receive(); }

} // namespace adbcpp
