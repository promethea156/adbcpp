#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "adbcpp/connection.hpp"
#include "adbcpp/error.hpp"
#include "adbcpp/export.hpp"

namespace adbcpp
{

/// The result of running a shell command on the device.
struct ADBCPP_API CommandResult
{
    /// Combined standard output and standard error.
    std::string output;
    /// The command's exit code.
    std::uint8_t exit_code = 0;
    /// Whether the device reported the command worked. For a plain command this is
    /// `exit_code == 0`.
    bool success = false;
};

/// Which shell service `run` opens.
enum class ShellProtocol
{
    /// `shell,v2,raw` when the device advertised `shell_v2`, and the v1 `shell`
    /// otherwise. This is the default, and matches what adb does.
    Auto,
    /// The v2 protocol. It reports the exit code, but the device must have
    /// advertised `shell_v2`.
    V2,
    /// The v1 `shell` service. It needs no feature, but reports no exit code, so
    /// `CommandResult::exit_code` is always 0, exactly as adb's own v1 path does.
    V1
};

/**
 * @brief Runs a shell command on the device and returns its result.
 *
 * By default the command is sent over the `shell,v2,raw` service when the device
 * advertised `shell_v2`, so the device's output arrives as shell_v2 packets which are
 * reassembled here, and the exit code is read from the exit packet. The `shell_v2`
 * feature must have been negotiated in the CNXN banners for the device to offer it;
 * the service is documented in AOSP's `shell_protocol.h`:
 *
 *   https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/shell_protocol.h
 *
 * A device that did not advertise `shell_v2` gets the plain `shell:<command>`
 * service instead, whose output is the raw stream and whose exit code is always 0
 * (blocker 31 in `04-blockers.md`). `protocol` forces one form or the other.
 *
 * The `raw` suffix asks the device to run the command directly instead of
 * through a login shell, and is the form adb itself uses for `adb shell`.
 */
Result<CommandResult> ADBCPP_API run(Connection &connection, std::string_view command,
                                     ShellProtocol protocol = ShellProtocol::Auto);

} // namespace adbcpp
