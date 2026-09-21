#include "adbcpp/shell.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

#include "adbcpp/stream.hpp"
#include "protocol/byte_order.hpp"

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
using protocol::read_u32_le;

} // namespace

// The v1 `shell:<command>` service: the device's combined output arrives as raw
// WRTE payloads with no packet framing, and there is no exit packet, so the exit
// code is always 0. This matches adb's own v1 path, which also reports 0.
Result<CommandResult> run_v1(Connection &connection, std::string_view command)
{
    auto stream = Stream::open(connection, "shell:" + std::string(command));
    if (!stream)
    {
        return tl::unexpected(stream.error());
    }
    const auto raw = stream->read_all();
    if (!raw)
    {
        return tl::unexpected(raw.error());
    }

    CommandResult result;
    // The v1 service does not separate the streams, so only the combined output
    // is filled; `standard_output` and `error_output` stay empty.
    result.output.assign(reinterpret_cast<const char *>(raw->data()), raw->size());
    result.success = true;
    return result;
}

// The v2 `shell,v2,raw` service: the device's output arrives as shell_v2 packets
// and the exit code is the exit packet's data.
Result<CommandResult> run_v2(Connection &connection, std::string_view command)
{
    // The service string is the whole command line, for example
    // `shell,v2,raw:echo hello`. adbd runs it and streams the output back.
    auto stream = Stream::open(connection, "shell,v2,raw:" + std::string(command));
    if (!stream)
    {
        return tl::unexpected(stream.error());
    }
    const auto raw = stream->read_all();
    if (!raw)
    {
        return tl::unexpected(raw.error());
    }

    // Each shell_v2 packet is a 1-byte id followed by a 4-byte little-endian
    // length and then `length` bytes of data.
    CommandResult result;
    std::size_t offset = 0;
    while (offset + 5 <= raw->size())
    {
        const auto id = std::to_integer<std::uint8_t>((*raw)[offset]);
        const std::uint32_t length = read_u32_le(raw->data() + offset + 1);
        offset += 5;
        if (offset + length > raw->size())
        {
            break;
        }
        if (id == kStdout)
        {
            // Each stream is kept separately as well as appended to the combined
            // output, so a caller can tell which stream a line came from.
            result.standard_output.append(reinterpret_cast<const char *>(raw->data() + offset), length);
            result.output.append(reinterpret_cast<const char *>(raw->data() + offset), length);
        }
        else if (id == kStderr)
        {
            result.error_output.append(reinterpret_cast<const char *>(raw->data() + offset), length);
            // stdout and stderr are interleaved in the combined output in the
            // order the device produced them, which is what `adb shell` does too.
            result.output.append(reinterpret_cast<const char *>(raw->data() + offset), length);
        }
        else if (id == kExit)
        {
            // The exit packet's data is a single byte holding the exit status, and
            // its length is always 1. adbd writes it with
            //
            //   output_->data()[0] = exit_code;
            //   output_->Write(ShellProtocol::kIdExit, 1);
            //
            // in `daemon/shell_service.cpp`, so the length is the size of the
            // status, not the status itself. Reading the length here was blocker 19
            // and reported every command as exiting with 1.
            if (length >= 1)
            {
                result.exit_code = static_cast<std::uint8_t>((*raw)[offset]);
            }
        }
        offset += length;
    }
    result.success = result.exit_code == 0;
    return result;
}

Result<CommandResult> run(Connection &connection, std::string_view command, ShellProtocol protocol)
{
    const bool use_v2 =
        protocol == ShellProtocol::V2 || (protocol == ShellProtocol::Auto && connection.supports_feature("shell_v2"));
    if (!use_v2)
    {
        return run_v1(connection, command);
    }
    return run_v2(connection, command);
}

} // namespace adbcpp
