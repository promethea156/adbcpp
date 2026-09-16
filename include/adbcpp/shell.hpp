#pragma once

#include <string>
#include <string_view>

#include "adbcpp/connection.hpp"
#include "adbcpp/export.hpp"

namespace adbcpp {

/// Runs a shell command on the device and returns its combined output.
std::string ADBCPP_API run(Connection &connection, std::string_view command);

} // namespace adbcpp
