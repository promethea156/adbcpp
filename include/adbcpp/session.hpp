#pragma once

#include <cstddef>
#include <span>
#include <vector>

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
    void send(const protocol::Message &header,
              std::span<const std::byte> payload = {});

    /// Reads exactly one header and its payload.
    Frame receive();

private:
    Transport *transport_;
};

} // namespace adbcpp
