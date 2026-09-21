#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include "adbcpp/error.hpp"
#include "adbcpp/export.hpp"
#include "adbcpp/log.hpp"
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
/// The list names only the features the library acts on: `shell_v2` for `run`,
/// and `ls_v2`/`stat_v2` for the v2 `LIST`/`STAT` forms. AOSP's
/// `supported_features()` is longer, and the extra names claim services the library
/// never opens (for example `sendrecv_v2`, when `pull`/`push` always send the v1
/// forms), so a peer would rely on them in vain. See blocker 32 in
/// `04-blockers.md`.
inline constexpr std::string_view kSystemIdentity = "host::features=shell_v2,stat_v2,ls_v2";

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
 * The CNXN handshake is performed by `connect`, following the sequence in
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
 *
 * A `Connection` is not thread-safe. `send` and `receive` share the session's
 * transport and the stream multiplexing state, so concurrent calls must be
 * serialized by the caller. This matches `Transport`, whose implementations are
 * not required to be thread-safe either.
 *
 * A `Connection` borrows its `Transport` and does not own it, so the transport
 * must outlive the connection. `close` closes the transport and marks the
 * connection dead, after which `send` and `receive` report a `Transport` error
 * rather than touching the closed transport.
 */
class ADBCPP_API Connection
{
public:
    /// Signs an AUTH token with the private key, or reports why it cannot.
    using Signer = std::function<Result<std::vector<std::byte>>(std::span<const std::byte>)>;

    /**
     * @brief Connects to `transport` and performs the handshake above.
     *
     * The handshake can fail, and a constructor cannot report that, so this is a
     * named factory and the constructor is private.
     */
    static Result<Connection> connect(Transport &transport, std::span<const std::byte> public_key = {},
                                      Signer signer = {}, bool advertise_delayed_ack = false);

    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;

    /// A connection is returned by value, so it moves.
    Connection(Connection &&) noexcept = default;
    Connection &operator=(Connection &&) noexcept = default;

    /// Sends a header and optional payload on the connection.
    Status send(const protocol::Message &header, std::span<const std::byte> payload = {});

    /// Receives the next frame from the connection.
    Result<Frame> receive();

    /// Closes the transport and marks the connection dead. Calling it twice is
    /// harmless.
    ///
    /// The connection borrows the transport and does not own it, so the transport
    /// must still outlive the connection. After this, `send` and `receive` return
    /// an `ErrorCode::Transport` error rather than touching the closed transport.
    void close() noexcept
    {
        session_.close();
    }

    /// Whether @ref close has been called.
    bool is_open() const noexcept
    {
        return session_.is_open();
    }

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

    /// Whether the device advertised `feature` in its CNXN banner.
    ///
    /// The device's feature list selects the protocol variants, for example
    /// `ls_v2` for the v2 LIST/DENT form or `stat_v2` for STAT. The match is
    /// exact, because a plain substring search would treat `recv_v2` as matching
    /// `sendrecv_v2`.
    bool supports_feature(std::string_view feature) const noexcept;

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

    /// The device's serial, as `adb devices` prints it, or empty when it has none.
    ///
    /// A USB device's serial is its `iSerial` descriptor, and a TCP transport's is
    /// its endpoint, both reported by the transport. This falls back to the CNXN
    /// banner's `serialno` field for a device whose transport has none.
    ///
    /// The view points into the transport or the parsed banner, so it is valid only
    /// for as long as this connection.
    std::string_view device_serial() const noexcept
    {
        if (!session_.serial().empty())
        {
            return session_.serial();
        }
        return device_serial_;
    }

private:
    // Connecting is done by `connect`, which owns the handshake, so the
    // constructor only holds the session.
    explicit Connection(Transport &transport);

    Session session_;
    std::uint32_t device_version_ = 0;
    std::uint32_t max_data_ = 0;
    // adb reserves local id 1; streams start at 2.
    std::uint32_t next_local_id_ = 2;
    // The device's `features=` list, parsed from its CNXN banner.
    std::vector<std::string> features_;
    // The device's banner `serialno`, parsed from its CNXN banner.
    std::string device_serial_;
    bool delayed_ack_ = false;
    bool requested_authorization_ = false;
};

/**
 * @brief Opens a transport with `open` and connects, retrying the whole open and
 * handshake with a bounded exponential backoff.
 *
 * A device can reset its USB 3 link right after the open and stall the first
 * write, and a dropped TCP link needs a new transport, so the open and the
 * handshake are retried together (blocker 29 in `04-blockers.md`). The retry is at
 * the open rather than in `Session`, because a device that re-enumerates
 * invalidates the handle. `transport` is where the opened transport lives and is
 * emplaced before each connect, so the returned connection borrows it and the
 * transport must outlive the connection; on a failed connect it is reset again.
 *
 * `open` is a callable returning `Result<TransportT>`, for example
 * `[] { return UsbTransport::open(id); }`. The remaining arguments are passed to
 * `Connection::connect` unchanged.
 *
 * The delay before the second attempt is `backoff` and doubles for each later
 * attempt, so the wait is bounded by `attempts` and the total stays finite. This is
 * also how a link is reconnected: close the connection, then call this again, which
 * replaces the transport in `transport` and repeats the handshake.
 *
 * @return the connected connection, or the last error once every attempt failed.
 */
template <typename TransportT, typename Open>
Result<Connection> connect_with_retry(Open open, std::optional<TransportT> &transport,
                                      std::span<const std::byte> public_key = {}, Connection::Signer signer = {},
                                      bool advertise_delayed_ack = false, int attempts = 5,
                                      std::chrono::milliseconds backoff = std::chrono::milliseconds{250})
{
    static_assert(std::is_same_v<std::invoke_result_t<Open>, Result<TransportT>>,
                  "open must return a Result<TransportT>");

    Result<Connection> connection = tl::unexpected(Error{ErrorCode::Transport, "the transport was not opened"});
    for (int attempt = 0; attempt < attempts; ++attempt)
    {
        if (attempt > 0)
        {
            // A bounded doubling, so the wait grows but cannot overflow.
            const int shift = std::min(attempt - 1, 20);
            std::this_thread::sleep_for(backoff * (1 << shift));
        }

        auto opened = open();
        if (!opened)
        {
            connection = tl::unexpected(opened.error());
            log(LogLevel::Warning, "connect attempt " + std::to_string(attempt + 1) + " of " +
                                       std::to_string(attempts) + " failed: " + opened.error().message);
            continue;
        }
        // The connection keeps a pointer to the transport, so the transport is
        // emplaced before `connect` and never moved once it holds a device.
        transport.emplace(std::move(*opened));

        auto connected = Connection::connect(*transport, public_key, signer, advertise_delayed_ack);
        if (!connected)
        {
            connection = tl::unexpected(connected.error());
            transport.reset();
            log(LogLevel::Warning, "connect attempt " + std::to_string(attempt + 1) + " of " +
                                       std::to_string(attempts) + " failed: " + connected.error().message);
            continue;
        }
        return std::move(*connected);
    }
    return connection;
}

} // namespace adbcpp
