#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "adbcpp/error.hpp"
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
 * `magic` is always the bitwise inverse of `command`. Together with the payload
 * checksum it is how the protocol detects a desynchronized stream: the document
 * requires that a bad header or payload close the connection, because the protocol
 * depends on shared state and cannot recover from a framing error.
 */
struct ADBCPP_API Message
{
    std::uint32_t command = 0;
    std::uint32_t arg0 = 0;
    std::uint32_t arg1 = 0;
    /// The payload length. It must equal the size of the payload sent with this
    /// header, because the device trusts it to know how many bytes follow.
    /// `Session::send` checks the two agree before it writes anything; a header
    /// that disagrees would otherwise leave the device waiting for bytes that
    /// never arrive (blocker 13 in `04-blockers.md`).
    std::uint32_t data_length = 0;
    /// The payload checksum. `docs/dev/protocol.md` calls this field
    /// `data_crc32`, but AOSP's `amessage` calls it `data_check` and computes a
    /// plain byte sum, not a CRC-32.
    std::uint32_t data_check = 0;
    std::uint32_t magic = 0;

    /// Returns the magic value for a given command (`command ^ 0xFFFFFFFF`).
    ///
    /// The protocol defines `magic = command ^ 0xffffffff`; a receiver rejects a
    /// header whose magic does not match its command.
    static std::uint32_t compute_magic(std::uint32_t command) noexcept;

    /// Returns the sum of the payload bytes, which is AOSP's `data_check`.
    ///
    /// AOSP's `calculate_apacket_checksum` adds the payload bytes rather than
    /// computing a CRC-32, and `send_packet` computes it only while the protocol
    /// is below `A_VERSION_SKIP_CHECKSUM`: adb sets it on the handshake's CNXN
    /// and AUTH messages, which is where adbd verifies it. A device on the
    /// current protocol sends zero, so `Session::receive` verifies it only when it
    /// is non-zero.
    static std::uint32_t compute_checksum(std::span<const std::byte> data) noexcept;

    /// Returns `payload.size()` as the 32-bit `data_length` field.
    ///
    /// The header stores the length in a single 32-bit word, so a payload that
    /// does not fit cannot be framed. That is an `ErrorCode::InvalidArgument`
    /// rather than a truncated length, which would silently desynchronize the
    /// stream.
    static Result<std::uint32_t> data_length_of(std::span<const std::byte> payload);

    /// Serializes the header into 24 little-endian bytes.
    std::array<std::byte, kMessageHeaderSize> encode() const noexcept;

    /// Parses a 24-byte little-endian header.
    static Message decode(std::span<const std::byte, kMessageHeaderSize> bytes) noexcept;
};

} // namespace adbcpp::protocol
