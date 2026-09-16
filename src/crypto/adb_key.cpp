#include "adbcpp/crypto/adb_key.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <mbedtls/base64.h>
#include <mbedtls/bignum.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/rsa.h>

namespace adbcpp::crypto {
namespace {

constexpr std::size_t kModulusBytes = 256;
constexpr std::size_t kBlobSize = 4 + 4 + kModulusBytes + kModulusBytes + 4;

void write_u32_le(std::byte *out, std::uint32_t value) {
  out[0] = static_cast<std::byte>(value & 0xFFu);
  out[1] = static_cast<std::byte>((value >> 8) & 0xFFu);
  out[2] = static_cast<std::byte>((value >> 16) & 0xFFu);
  out[3] = static_cast<std::byte>((value >> 24) & 0xFFu);
}

// Returns the multiplicative inverse of `value` modulo 2^32.
std::uint32_t inverse_mod_2_32(std::uint32_t value) {
  std::uint32_t inverse = 1;
  for (int i = 0; i < 5; ++i) {
    inverse *= 2u - value * inverse;
  }
  return inverse;
}

// Writes `value` as little-endian bytes, right-aligned in `out`.
void to_little_endian(const mbedtls_mpi &value, std::span<std::byte> out) {
  std::vector<unsigned char> big_endian(out.size(), 0);
  if (mbedtls_mpi_write_binary(&value, big_endian.data(),
                                big_endian.size()) != 0) {
    throw std::runtime_error("adbcpp: failed to serialize an RSA component");
  }
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<std::byte>(big_endian[out.size() - 1 - i]);
  }
}

} // namespace

std::string generate_public_key() {
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_rsa_context rsa;
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&drbg);
  mbedtls_rsa_init(&rsa);

  int rc = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, nullptr,
                                 0);
  if (rc == 0) {
    rc = mbedtls_rsa_gen_key(&rsa, mbedtls_ctr_drbg_random, &drbg, 2048,
                            65537);
  }
  if (rc != 0) {
    mbedtls_rsa_free(&rsa);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    throw std::runtime_error("adbcpp: failed to generate an RSA key");
  }

  mbedtls_mpi n;
  mbedtls_mpi e;
  mbedtls_mpi rr;
  mbedtls_mpi_init(&n);
  mbedtls_mpi_init(&e);
  mbedtls_mpi_init(&rr);

  std::array<std::byte, kBlobSize> blob{};
  rc = mbedtls_rsa_export(&rsa, &n, nullptr, nullptr, nullptr, &e);
  if (rc == 0) {
    rc = mbedtls_mpi_lset(&rr, 1);
  }
  if (rc == 0) {
    rc = mbedtls_mpi_shift_l(&rr, 4096);
  }
  if (rc == 0) {
    rc = mbedtls_mpi_mod_mpi(&rr, &rr, &n);
  }
  if (rc != 0) {
    mbedtls_mpi_free(&rr);
    mbedtls_mpi_free(&e);
    mbedtls_mpi_free(&n);
    mbedtls_rsa_free(&rsa);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    throw std::runtime_error("adbcpp: failed to derive RSA key parameters");
  }

  auto modulus = std::span(blob).subspan(8, kModulusBytes);
  to_little_endian(n, modulus);
  to_little_endian(rr, std::span(blob).subspan(8 + kModulusBytes, kModulusBytes));
  to_little_endian(e, std::span(blob).subspan(8 + 2 * kModulusBytes, 4));

  const std::uint32_t n0 = static_cast<std::uint32_t>(
                               std::to_integer<std::uint8_t>(modulus[0])) |
                           (static_cast<std::uint32_t>(
                                std::to_integer<std::uint8_t>(modulus[1]))
                            << 8) |
                           (static_cast<std::uint32_t>(
                                std::to_integer<std::uint8_t>(modulus[2]))
                            << 16) |
                           (static_cast<std::uint32_t>(
                                std::to_integer<std::uint8_t>(modulus[3]))
                            << 24);
  write_u32_le(blob.data() + 0, static_cast<std::uint32_t>(kModulusBytes));
  write_u32_le(blob.data() + 4, 0u - inverse_mod_2_32(n0));

  mbedtls_mpi_free(&rr);
  mbedtls_mpi_free(&e);
  mbedtls_mpi_free(&n);
  mbedtls_rsa_free(&rsa);
  mbedtls_ctr_drbg_free(&drbg);
  mbedtls_entropy_free(&entropy);

  std::vector<unsigned char> encoded(4 * ((kBlobSize + 2) / 3) + 1, 0);
  std::size_t length = 0;
  if (mbedtls_base64_encode(
          encoded.data(), encoded.size(), &length,
          reinterpret_cast<const unsigned char *>(blob.data()),
          blob.size()) != 0) {
    throw std::runtime_error("adbcpp: failed to encode the public key");
  }

  std::string key(reinterpret_cast<const char *>(encoded.data()), length);
  key += " adbcpp@localhost";
  return key;
}

} // namespace adbcpp::crypto
