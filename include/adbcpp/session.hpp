#pragma once

#include "adbcpp/export.hpp"
#include "adbcpp/protocol/message.hpp"
#include "adbcpp/transport.hpp"

namespace adbcpp {

/**
 * @brief A framed ADB session over a transport.
 *
 * For now a session only sends and receives message headers. Payload handling,
 * the connection handshake, and authentication are added in later slices.
 */
class ADBCPP_API Session {
public:
  explicit Session(Transport &transport) noexcept;

  /// Writes `message` to the underlying transport.
  void send(const protocol::Message &message);

  /// Reads exactly one message header from the underlying transport.
  protocol::Message receive();

private:
  Transport *transport_;
};

} // namespace adbcpp
