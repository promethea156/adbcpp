#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "adbcpp/connection.hpp"
#include "adbcpp/export.hpp"

namespace adbcpp {

/// The result of running a shell command on the device.
struct ADBCPP_API CommandResult {
  /// Combined standard output and standard error.
  std::string output;
  /// The command's exit code.
  std::uint8_t exit_code = 0;
};

/**
 * @brief Runs a shell command on the device and returns its result.
 *
 * The command is sent over the `shell,v2,raw` service, so the device's output
 * arrives as shell_v2 packets which are reassembled here.
 */
CommandResult ADBCPP_API run(Connection &connection, std::string_view command);

} // namespace adbcpp
