#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
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
    "app_info,server_status,track_mdns";

/// Feature appended to @ref kSystemIdentity when delayed acknowledgements are enabled.
inline constexpr std::string_view kDelayedAckFeature = "delayed_ack";

/**
 * @brief An authenticated ADB connection to a device.
 *
 * The CNXN handshake is performed on construction. If the device requests
 * authentication, the token is signed with `signer` and returned as an AUTH type 2
   * message, exactly like adb; when no signer is given the public key is sent
   * instead (AUTH type 3), which opens the on-device approval prompt.
   *
   * When `advertise_delayed_ack` is set, `delayed_ack` is added to the advertised
   * feature list and OPEN messages send a non-zero receive window.
   */
class ADBCPP_API Connection {
public:
  /// Signs an AUTH token with the private key.
  using Signer =
      std::function<std::vector<std::byte>(std::span<const std::byte>)>;

  explicit Connection(Transport &transport,
                      std::span<const std::byte> public_key = {},
                      Signer signer = {},
                      bool advertise_delayed_ack = false);

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

  /// Whether delayed acknowledgements were negotiated with the device.
  bool supports_delayed_ack() const noexcept { return delayed_ack_; }

  /// Whether the device rejected the signature and asked for authorization.
  bool requested_authorization() const noexcept {
    return requested_authorization_;
  }

private:
  Session session_;
  std::uint32_t device_version_ = 0;
  std::uint32_t max_data_ = 0;
  // adb reserves local id 1; streams start at 2.
  std::uint32_t next_local_id_ = 2;
  bool delayed_ack_ = false;
  bool requested_authorization_ = false;
};

} // namespace adbcpp
