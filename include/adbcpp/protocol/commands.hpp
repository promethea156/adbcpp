#pragma once

#include <cstdint>

namespace adbcpp::protocol {

/// Packs four ASCII characters into an ADB command identifier.
constexpr std::uint32_t make_command(char a, char b, char c, char d) noexcept {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24);
}

inline constexpr std::uint32_t kCnxn = make_command('C', 'N', 'X', 'N');
inline constexpr std::uint32_t kAuth = make_command('A', 'U', 'T', 'H');
inline constexpr std::uint32_t kOpen = make_command('O', 'P', 'E', 'N');
inline constexpr std::uint32_t kOkay = make_command('O', 'K', 'A', 'Y');
inline constexpr std::uint32_t kClse = make_command('C', 'L', 'S', 'E');
inline constexpr std::uint32_t kWrte = make_command('W', 'R', 'T', 'E');
inline constexpr std::uint32_t kSync = make_command('S', 'Y', 'N', 'C');

/// ADB protocol version advertised in the CNXN message.
inline constexpr std::uint32_t kVersion = 0x01000001u;

/// Maximum payload size advertised in the CNXN message.
inline constexpr std::uint32_t kMaxData = 256 * 1024;

/// AUTH payload type values (the `arg0` field of an AUTH message).
inline constexpr std::uint32_t kAuthToken = 1;
inline constexpr std::uint32_t kAuthSignature = 2;
inline constexpr std::uint32_t kAuthPublicKey = 3;

} // namespace adbcpp::protocol
