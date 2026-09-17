#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "adbcpp/export.hpp"

namespace adbcpp::crypto {

/**
 * @brief An ADB RSA-2048 key pair.
 *
 * The key pair is stored in ADB's own format: the private key at
 * `~/.android/adbkey` and the public key at `~/.android/adbkey.pub`. Reusing
 * the same key means the device only asks the user to approve it once, and the
 * private key lets us answer an AUTH token by signing it (AUTH type 2) exactly
 * like adb does.
 */
class ADBCPP_API Key {
public:
  ~Key();
  Key(Key &&) noexcept;
  Key &operator=(Key &&) noexcept;
  Key(const Key &) = delete;
  Key &operator=(const Key &) = delete;

  /// Generates a new RSA-2048 key pair.
  static Key generate();

  /// Loads the key pair from `~/.android/adbkey`, generating it if absent.
  static Key load_or_generate();

  /// The ADB public key string, as sent with AUTH type 3.
  const std::string &public_key() const noexcept;

  /// The MD5 fingerprint of the public key, as shown on the device.
  std::string fingerprint() const;

  /**
   * Signs `token` with the private key (PKCS#1 v1.5, SHA-1).
   *
   * The token is the SHA-1 digest itself and is signed directly, exactly like
   * adb's `RSA_sign(NID_sha1, token, ...)`; it is not re-hashed.
   */
  std::vector<std::byte> sign(std::span<const std::byte> token) const;

private:
  Key();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace adbcpp::crypto
