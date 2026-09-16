#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "adbcpp/export.hpp"

namespace adbcpp::protocol {

/// Size in bytes of an ADB message header.
inline constexpr std::size_t kMessageHeaderSize = 24;

/**
 * @brief An ADB message header.
 *
 * The header is serialized in little-endian byte order. The `magic` field is
 * always the bitwise inverse of `command`.
 */
struct ADBCPP_API Message {
  std::uint32_t command = 0;
  std::uint32_t arg0 = 0;
  std::uint32_t arg1 = 0;
  std::uint32_t data_length = 0;
  std::uint32_t data_crc32 = 0;
  std::uint32_t magic = 0;

  /// Returns the magic value for a given command (`command ^ 0xFFFFFFFF`).
  static std::uint32_t compute_magic(std::uint32_t command) noexcept;

  /// Returns the standard CRC32 of `data`.
  static std::uint32_t compute_crc32(std::span<const std::byte> data) noexcept;

  /// Serializes the header into 24 little-endian bytes.
  std::array<std::byte, kMessageHeaderSize> encode() const noexcept;

  /// Parses a 24-byte little-endian header.
  static Message decode(
      std::span<const std::byte, kMessageHeaderSize> bytes) noexcept;
};

} // namespace adbcpp::protocol
