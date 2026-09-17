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
 * ADB reuses adb's own key files: the private key at `~/.android/adbkey` and the
 * public key at `~/.android/adbkey.pub`. Sharing them means the device treats
 * this library as the same computer adb already authorized, so it does not show the
 * approval prompt. The private key lets us answer an AUTH token by signing it
 * (AUTH type 2) exactly like adb does.
 *
 * The private key is a PKCS#8 PEM (`adb`'s format; the older raw
 * `RSAPrivateKey` is not used). The public key is **not** a standard DER
 * `SubjectPublicKeyInfo`: it is AOSP's custom `RSAPublicKey` structure, which is
 * also what adbd expects in an AUTH type 3 payload. Both formats are described
 * in `adb_key.cpp` and in blockers 8 and 9 of `04-blockers.md`.
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
  ///
  /// This is the MD5 of the decoded 524-byte blob, formatted as uppercase
  /// colon-separated hex. It is what the device's "USB debugging authorized
  /// computers" list shows, so it is useful for checking that this library and adb
  /// use the same key. adb's own log prints a SHA-256 of the DER
  /// `SubjectPublicKeyInfo` instead, so the two fingerprints do not match.
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
