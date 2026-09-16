#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "adbcpp/export.hpp"
#include "adbcpp/protocol/message.hpp"
#include "adbcpp/session.hpp"
#include "adbcpp/transport.hpp"

namespace adbcpp {

/// System identity string sent in the initial CNXN message.
inline constexpr std::string_view kSystemIdentity =
    "host::features=shell_v2,cmd,stat_v2,ls_v2,fixed_push_mkdir,apex,abb,"
    "fixed_push_symlink_timestamp,abb_exec,remount_shell,track_app,sendrecv_v2,"
    "sendrecv_v2_brotli,sendrecv_v2_lz4,sendrecv_v2_zstd,"
    "sendrecv_v2_dry_run_send,openscreen_mdns,devicetracker_proto_format,devraw,"
    "app_info,server_status,track_mdns,delayed_ack";

/**
 * @brief An authenticated ADB connection to a device.
 *
 * The CNXN handshake is performed on construction. If the device requests
 * authentication, the supplied public key is sent with an AUTH type 3 message,
 * which opens the "allow USB debugging" prompt on the device.
 */
class ADBCPP_API Connection {
public:
  explicit Connection(Transport &transport,
                      std::span<const std::byte> public_key = {});

  /// Sends a header and optional payload on the connection.
  void send(const protocol::Message &header,
            std::span<const std::byte> payload = {});

  /// Receives the next frame from the connection.
  Frame receive();

  /// Allocates a unique, non-zero local stream id.
  std::uint32_t allocate_local_id() noexcept { return next_local_id_++; }

  /// The protocol version reported by the device.
  std::uint32_t device_version() const noexcept { return device_version_; }

  /// The maximum payload size reported by the device.
  std::uint32_t max_data() const noexcept { return max_data_; }

private:
  Session session_;
  std::uint32_t device_version_ = 0;
  std::uint32_t max_data_ = 0;
  std::uint32_t next_local_id_ = 1;
};

} // namespace adbcpp
