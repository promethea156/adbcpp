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

namespace adbcpp
{

/// System identity string sent in the initial CNXN message.
///
/// `docs/dev/protocol.md` describes the CNXN payload as
/// "<systemtype>::<serialno>::<banner>", where systemtype is `host` for the
/// host side and the banner carries useful properties. In practice the host banner
/// is `host::features=<list>`: the feature list tells adbd what the host can do,
/// and adbd **resets its own feature set from it**, so an empty list makes the
/// device treat the host as supporting nothing (blocker 6 in `04-blockers.md`).
///
/// This list is copied from AOSP's `supported_features()` in `adb.cpp`; a USBPcap
/// capture of adb 37.0.1 confirmed it byte-for-byte, including the absence of
/// `openscreen_mdns` (which is an adbd feature, not a host one).
inline constexpr std::string_view kSystemIdentity =
    "host::features=shell_v2,cmd,stat_v2,ls_v2,fixed_push_mkdir,apex,abb,"
    "fixed_push_symlink_timestamp,abb_exec,remount_shell,track_app,sendrecv_v2,"
    "sendrecv_v2_brotli,sendrecv_v2_lz4,sendrecv_v2_zstd,"
    "sendrecv_v2_dry_run_send,devicetracker_proto_format,devraw,"
    "app_info,server_status,track_mdns";

/// Feature appended to @ref kSystemIdentity when delayed acknowledgements are enabled.
///
/// The feature name is the string adb looks for in the device's banner to decide
/// whether to use the "available send bytes" window on `OPEN`/`OKAY`. See
/// `docs/dev/delayed_ack.md` and blocker 12 in `04-blockers.md` for why it is off
/// by default: advertising it on a transport that does not really implement it made
/// the device close the stream.
inline constexpr std::string_view kDelayedAckFeature = "delayed_ack";

/**
 * @brief An authenticated ADB connection to a device.
 *
 * The CNXN handshake is performed on construction, following the sequence in
 * `docs/dev/protocol.md`:
 *
 *   https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/docs/dev/protocol.md
 *
 * 1. The host sends CNXN(version, maxdata, host banner).
 * 2. If the device needs authentication it replies AUTH(type 1, token). The token
 *    is signed with `signer` and sent back as AUTH type 2, exactly like adb.
 * 3. If the device rejects the signature it sends another AUTH token. The host
 *    then sends its public key as AUTH type 3, which opens the on-device
 *    approval prompt. `requested_authorization()` reports that this happened.
 * 4. The device finishes the handshake with its own CNXN, whose banner carries
 *    the device's features and protocol version.
 *
 * The signing and key format are in `crypto/adb_key.hpp`; blockers 10 and 16 in
 * `04-blockers.md` explain why the token must be signed as-is.
 *
 * When `advertise_delayed_ack` is set, `delayed_ack` is added to the advertised
 * feature list and OPEN messages send a non-zero receive window.
 */
class ADBCPP_API Connection
{
public:
    /// Signs an AUTH token with the private key.
    using Signer = std::function<std::vector<std::byte>(std::span<const std::byte>)>;

    explicit Connection(Transport &transport, std::span<const std::byte> public_key = {}, Signer signer = {},
                        bool advertise_delayed_ack = false);

    /// Sends a header and optional payload on the connection.
    void send(const protocol::Message &header, std::span<const std::byte> payload = {});

    /// Receives the next frame from the connection.
    Frame receive();

    /// Allocates a unique, non-zero local stream id.
    std::uint32_t allocate_local_id() noexcept
    {
        return next_local_id_++;
    }

    /// The protocol version reported by the device.
    std::uint32_t device_version() const noexcept
    {
        return device_version_;
    }

    /// The maximum payload size reported by the device.
    std::uint32_t max_data() const noexcept
    {
        return max_data_;
    }

    /// Whether delayed acknowledgements were negotiated with the device.
    bool supports_delayed_ack() const noexcept
    {
        return delayed_ack_;
    }

    /// Whether the device rejected the signature and asked for authorization.
    bool requested_authorization() const noexcept
    {
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
