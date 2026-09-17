#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "adbcpp/export.hpp"

namespace adbcpp::protocol
{

/// Size in bytes of an ADB message header.
inline constexpr std::size_t kMessageHeaderSize = 24;

/**
 * @brief An ADB message header.
 *
 * This mirrors AOSP's `struct amessage` (the wire-level `struct message` from
 * `docs/dev/protocol.md`): six 32-bit words, sent little-endian:
 *
 *   https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/docs/dev/protocol.md
 *
 * The `arg0`/`arg1` meaning depends on `command`; for example a CNXN uses
 * `arg0` as the protocol version and `arg1` as the maximum payload size, while a
 * WRTE uses them as the stream's local and remote ids.
 *
 * `magic` is always the bitwise inverse of `command`. Together with the CRC it is
 * how the protocol detects a desynchronized stream: the document requires that a
 * bad header or payload close the connection, because the protocol depends on shared
 * state and cannot recover from a framing error.
 */
struct ADBCPP_API Message
{
    std::uint32_t command = 0;
    std::uint32_t arg0 = 0;
    std::uint32_t arg1 = 0;
    std::uint32_t data_length = 0;
    std::uint32_t data_crc32 = 0;
    std::uint32_t magic = 0;

    /// Returns the magic value for a given command (`command ^ 0xFFFFFFFF`).
    ///
    /// The protocol defines `magic = command ^ 0xffffffff`; a receiver rejects a
    /// header whose magic does not match its command.
    static std::uint32_t compute_magic(std::uint32_t command) noexcept;

    /// Returns the standard CRC32 of `data`.
    ///
    /// This is the usual zlib CRC-32 (reflected polynomial `0xEDB88320`, initial
    /// and final value `0xFFFFFFFF`). AOSP's `apacket` computes the same CRC over
    /// the payload, although a later change made the CRC advisory: the receiving
    /// side no longer verifies it, since USB and TCP already have their own
    /// integrity checks (see `docs/dev/delayed_ack.md`).
    static std::uint32_t compute_crc32(std::span<const std::byte> data) noexcept;

    /// Returns `payload.size()` as the 32-bit `data_length` field.
    ///
    /// The header stores the length in a single 32-bit word, so a payload that
    /// does not fit cannot be framed. This throws rather than truncating the length
    /// and silently desynchronizing the stream.
    static std::uint32_t data_length_of(std::span<const std::byte> payload);

    /// Serializes the header into 24 little-endian bytes.
    std::array<std::byte, kMessageHeaderSize> encode() const noexcept;

    /// Parses a 24-byte little-endian header.
    static Message decode(std::span<const std::byte, kMessageHeaderSize> bytes) noexcept;
};

} // namespace adbcpp::protocol
