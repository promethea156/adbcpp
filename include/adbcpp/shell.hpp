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

/**
 * @brief Runs a shell command on the device and returns its result.
 *
 * The command is sent over the `shell,v2,raw` service, so the device's output
 * arrives as shell_v2 packets which are reassembled here. The `shell_v2` feature
 * must have been negotiated in the CNXN banners for the device to offer it; the
 * service is documented in AOSP's `shell_protocol.h`:
 *
 *   https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/shell_protocol.h
 *
 * The `raw` suffix asks the device to run the command directly instead of
 * through a login shell, and is the form adb itself uses for `adb shell`.
 */
Result<CommandResult> ADBCPP_API run(Connection &connection, std::string_view command);

} // namespace adbcpp
