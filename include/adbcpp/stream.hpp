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
 * Opening a stream sends an OPEN message for the given service and waits for the
 * device's OKAY. Reads collect WRTE payloads (acknowledging each with OKAY)
 * until the device closes the stream.
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
