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

} // namespace adbcpp::protocol
