#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "adbcpp/connection.hpp"
#include "adbcpp/export.hpp"

namespace adbcpp {

/**
 * @brief A logical stream to a service on the device.
 *
 * A stream multiplexes a named service (for example `shell,v2,raw:echo hello`)
 * over the one connection. This is the OPEN/READY/WRITE/CLOSE part of
 * `docs/dev/protocol.md`:
 *
 *   https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/docs/dev/protocol.md
 *
 * Opening a stream sends OPEN(local-id, window, "destination") and waits for the
 * device's OKAY(local-id, remote-id). The ids are relative to the sender, so the
 * `remote_id` here is the device's own id for this stream, and every later
 * WRTE/OKAY/CLSE carries both ids.
 *
 * Reads collect WRTE payloads, acknowledging each with OKAY, until the device
 * closes the stream. The window in OPEN's `arg1` is the delayed-acknowledgement
 * "available send bytes"; see `commands.hpp` and blocker 12 in `04-blockers.md`.
 */
class ADBCPP_API Stream {
public:
  Stream(Connection &connection, std::string_view service);
  ~Stream();

  Stream(const Stream &) = delete;
  Stream &operator=(const Stream &) = delete;

  /// Writes data to the remote stream.
  void write(std::span<const std::byte> data);

  /// Reads all output until the remote closes the stream.
  std::vector<std::byte> read_all();

  /// The service this stream was opened for.
  const std::string &service() const noexcept { return service_; }

private:
  Connection *connection_;
  std::string service_;
  std::uint32_t local_id_;
  std::uint32_t remote_id_ = 0;
  bool closed_ = false;
};

} // namespace adbcpp
