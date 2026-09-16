#pragma once

#include <string>

#include "adbcpp/export.hpp"

namespace adbcpp::crypto {

/// Generates an ADB RSA-2048 public key in the format sent with AUTH type 3.
std::string ADBCPP_API generate_public_key();

} // namespace adbcpp::crypto
