#pragma once

#include <string>

#include "adbcpp/export.hpp"

namespace adbcpp::crypto {

/// Generates an ADB RSA-2048 public key in the format sent with AUTH type 3.
std::string ADBCPP_API generate_public_key();

/**
 * @brief Returns the ADB public key, reusing an existing one when possible.
 *
 * If `~/.android/adbkey.pub` already exists it is read back; otherwise a new
 * key is generated and written there. Reusing the key means the device only asks
 * the user to approve it once.
 */
std::string ADBCPP_API load_or_generate_public_key();

} // namespace adbcpp::crypto
