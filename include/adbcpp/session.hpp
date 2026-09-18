#pragma once

#include <cstddef>
#include <span>
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
 */
class ADBCPP_API Session
{
public:
    explicit Session(Transport &transport) noexcept;

    /// Writes a header, then its payload as a separate transport write.
    Status send(const protocol::Message &header, std::span<const std::byte> payload = {});

    /// Reads exactly one header and its payload.
    ///
    /// The header is validated before its payload is read: `magic` must be the
    /// inverse of `command`, `data_length` must not exceed the protocol's maximum
    /// payload, and a non-zero `data_check` must match the payload. Any of these
    /// means the byte stream is desynchronized, and since the protocol cannot
    /// recover from a framing error the transport is closed and an
    /// `ErrorCode::Protocol` is returned.
    Result<Frame> receive();

private:
    Transport *transport_;
};

} // namespace adbcpp
