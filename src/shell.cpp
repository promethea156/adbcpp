#include "adbcpp/shell.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

#include "adbcpp/stream.hpp"

namespace adbcpp
{
namespace
{

// shell_v2 packet ids, from AOSP's `shell_protocol.h` (see `shell.hpp`).
// These arrive as the payload of WRTE messages on the stream; they are not ADB
// commands, so they have no 24-byte header of their own.
constexpr std::uint8_t kStdout = 1;
constexpr std::uint8_t kStderr = 2;
constexpr std::uint8_t kExit = 3;

// shell_v2 packets use 4-byte little-endian lengths, like the ADB header.
std::uint32_t read_u32_le(const std::byte *in) noexcept
{
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[0])) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[1])) << 8) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[2])) << 16) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[3])) << 24);
}

} // namespace

CommandResult run(Connection &connection, std::string_view command)
{
    // The service string is the whole command line, for example
    // `shell,v2,raw:echo hello`. adbd runs it and streams the output back.
    Stream stream(connection, "shell,v2,raw:" + std::string(command));
    const auto raw = stream.read_all();

    // Each shell_v2 packet is a 1-byte id followed by a 4-byte little-endian
    // length and then `length` bytes of data.
    CommandResult result;
    std::size_t offset = 0;
    while (offset + 5 <= raw.size())
    {
        const auto id = std::to_integer<std::uint8_t>(raw[offset]);
        const std::uint32_t length = read_u32_le(raw.data() + offset + 1);
        offset += 5;
        if (offset + length > raw.size())
        {
            break;
        }
        if (id == kStdout || id == kStderr)
        {
            // stdout and stderr are interleaved in the order the device produced
            // them; they are concatenated here, which is what `adb shell` does too.
            result.output.append(reinterpret_cast<const char *>(raw.data() + offset), length);
        }
        else if (id == kExit)
        {
            // For the exit packet the "length" field carries the exit status itself,
            // not the size of any data.
            result.exit_code = static_cast<std::uint8_t>(length);
        }
        offset += length;
    }
    return result;
}

} // namespace adbcpp
