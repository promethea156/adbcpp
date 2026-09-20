#pragma once

#include <cstddef>
#include <cstdint>

namespace adbcpp::protocol
{

/// Writes `value` as four little-endian bytes.
///
/// Every integer on the ADB wire, and on the `sync` and shell_v2 protocols that
/// travel inside it, is little-endian. These helpers keep the byte order explicit
/// instead of relying on the host's endianness, and are shared by all three rather
/// than copied into each.
inline void write_u32_le(std::byte *out, std::uint32_t value) noexcept
{
    out[0] = static_cast<std::byte>(value & 0xFFu);
    out[1] = static_cast<std::byte>((value >> 8) & 0xFFu);
    out[2] = static_cast<std::byte>((value >> 16) & 0xFFu);
    out[3] = static_cast<std::byte>((value >> 24) & 0xFFu);
}

/// Reads a 32-bit little-endian word.
inline std::uint32_t read_u32_le(const std::byte *in) noexcept
{
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[0])) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[1])) << 8) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[2])) << 16) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[3])) << 24);
}

} // namespace adbcpp::protocol
