#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "adbcpp/error.hpp"
#include "adbcpp/export.hpp"
#include "adbcpp/protocol/message.hpp"
#include "adbcpp/transport.hpp"

namespace adbcpp
{

/// A complete ADB message: a header plus its optional payload.
struct ADBCPP_API Frame
{
    protocol::Message header;
    std::vector<std::byte> payload;
};

/**
 * @brief A framed ADB session over a transport.
 *
 * This is the "transport layer" from `docs/dev/protocol.md`, the layer that deals
 * in complete messages (a 24-byte header plus its payload) rather than raw bytes:
 *
 *   https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/docs/dev/protocol.md
 *
 * A header and its payload are always written as two separate transport writes
 * (and read as two separate reads). This matches ADB-over-USB, where the header
 * and payload travel in distinct USB bulk transfers: AOSP's `usb_libusb.cpp` reads
 * a packet's header and payload with two separate bulk transfers. It is harmless
 * for byte-stream transports such as TCP, which is why the same `Session` works
 * over both.
 *
 * A `Session` borrows its transport and does not own it, so the transport must
 * outlive the session. @ref close closes the transport and marks the session
 * closed, after which @ref send and @ref receive report a `Transport` error rather
 * than touching the closed transport.
 */
class ADBCPP_API Session
{
public:
    explicit Session(Transport &transport) noexcept;

    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    /// A session is returned by value, so it moves. The moved-from session is
    /// marked closed, so its @ref close does not close the transport the
    /// moved-to session still uses.
    Session(Session &&other) noexcept;
    Session &operator=(Session &&other) noexcept;

    /**
     * @brief Writes a header, then its payload as a separate transport write.
     *
     * Returns an `ErrorCode::Transport` error when @ref close has been called,
     * rather than writing to the closed transport.
     */
    Status send(const protocol::Message &header, std::span<const std::byte> payload = {});

    /**
     * @brief Reads exactly one header and its payload.
     *
     * The header is validated before its payload is read: `magic` must be the
     * inverse of `command`, `data_length` must not exceed the protocol's maximum
     * payload, and a non-zero `data_check` must match the payload. Any of these
     * means the byte stream is desynchronized, and since the protocol cannot
     * recover from a framing error the session is closed and an
     * `ErrorCode::Protocol` is returned.
     *
     * Returns an `ErrorCode::Transport` error when @ref close has been called.
     */
    Result<Frame> receive();

    /// Closes the transport and marks the session closed. Calling it twice is
    /// harmless.
    ///
    /// The session borrows the transport and does not own it, so the transport must
    /// still outlive the session.
    void close() noexcept;

    /// Whether @ref close has been called.
    bool is_open() const noexcept
    {
        return !closed_;
    }

    /// The transport's serial, as `adb devices` prints it, or empty when the
    /// transport has none.
    std::string_view serial() const noexcept
    {
        return transport_->serial();
    }

private:
    /// Closes the session and returns `message` as a framing `Protocol` error.
    Error framing_error(std::string message);

    Transport *transport_;
    bool closed_ = false;
};

} // namespace adbcpp
