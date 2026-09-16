#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "adbcpp/export.hpp"
#include "adbcpp/protocol/message.hpp"
#include "adbcpp/transport.hpp"

namespace adbcpp {

/// A complete ADB message: a header plus its optional payload.
struct ADBCPP_API Frame {
  protocol::Message header;
  std::vector<std::byte> payload;
};

/**
 * @brief A framed ADB session over a transport.
 *
 * A header and its payload are always written as two separate transport writes
 * (and read as two separate reads). This matches the ADB-over-USB behaviour where
 * the header and payload travel in distinct USB transfers, and is harmless for
 * byte-stream transports such as TCP.
 */
class ADBCPP_API Session {
public:
  explicit Session(Transport &transport) noexcept;

  /// Writes a header, then its payload as a separate transport write.
  void send(const protocol::Message &header,
            std::span<const std::byte> payload = {});

  /// Reads exactly one header and its payload.
  Frame receive();

private:
  Transport *transport_;
};

} // namespace adbcpp
