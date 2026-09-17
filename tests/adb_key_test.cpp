#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <mbedtls/base64.h>
#include <mbedtls/bignum.h>
#include <mbedtls/rsa.h>

#include "adbcpp/crypto/adb_key.hpp"

TEST_CASE("generated public key has the ADB format", "[crypto]") {
  const auto key = adbcpp::crypto::Key::generate();
  const std::string &public_key = key.public_key();

  const auto space = public_key.find(' ');
  REQUIRE(space != std::string::npos);
  // 524-byte RSAPublicKey structure, base64-encoded.
  REQUIRE(space == 700);
  REQUIRE(public_key.substr(space) == " adbcpp@localhost");
}

TEST_CASE("generated public keys are unique", "[crypto]") {
  REQUIRE(adbcpp::crypto::Key::generate().public_key() !=
          adbcpp::crypto::Key::generate().public_key());
}

TEST_CASE("a token signature is 256 bytes", "[crypto]") {
  const auto key = adbcpp::crypto::Key::generate();
  const std::array<std::byte, 20> token{};

  const auto signature = key.sign(token);
  REQUIRE(signature.size() == 256);
}

TEST_CASE("a token signature verifies against the ADB public key",
          "[crypto]") {
  const auto key = adbcpp::crypto::Key::generate();

  const std::array<std::byte, 20> token{std::byte{0x01}, std::byte{0x02}};
  const auto signature = key.sign(token);

  const std::string &public_key = key.public_key();
  const std::string encoded = public_key.substr(0, public_key.find(' '));

  std::vector<unsigned char> blob(4 * ((524 + 2) / 3) + 1);
  std::size_t blob_size = 0;
  REQUIRE(mbedtls_base64_decode(
              blob.data(), blob.size(), &blob_size,
              reinterpret_cast<const unsigned char *>(encoded.data()),
              encoded.size()) == 0);
  REQUIRE(blob_size == 524);

  std::array<unsigned char, 256> modulus{};
  std::copy(blob.begin() + 8, blob.begin() + 8 + 256, modulus.begin());
  std::reverse(modulus.begin(), modulus.end());

  std::array<unsigned char, 4> exponent{};
  std::copy(blob.begin() + 8 + 512, blob.begin() + 8 + 516, exponent.begin());
  std::reverse(exponent.begin(), exponent.end());

  mbedtls_mpi n;
  mbedtls_mpi e;
  mbedtls_mpi_init(&n);
  mbedtls_mpi_init(&e);
  REQUIRE(mbedtls_mpi_read_binary(&n, modulus.data(), modulus.size()) == 0);
  REQUIRE(mbedtls_mpi_read_binary(&e, exponent.data(), exponent.size()) == 0);

  mbedtls_rsa_context rsa;
  mbedtls_rsa_init(&rsa);
  REQUIRE(mbedtls_rsa_import(&rsa, &n, nullptr, nullptr, nullptr, &e) == 0);
  REQUIRE(mbedtls_rsa_complete(&rsa) == 0);

  // The token is signed directly as the SHA-1 digest, exactly like adb's
  // RSA_sign(NID_sha1, token, token_size, ...); it is not re-hashed.
  REQUIRE(mbedtls_rsa_pkcs1_verify(
              &rsa, MBEDTLS_MD_SHA1, token.size(),
              reinterpret_cast<const unsigned char *>(token.data()),
              reinterpret_cast<const unsigned char *>(signature.data())) == 0);

  mbedtls_rsa_free(&rsa);
  mbedtls_mpi_free(&e);
  mbedtls_mpi_free(&n);
}

TEST_CASE("the ADB public key has valid Montgomery parameters", "[crypto]") {
  const auto key = adbcpp::crypto::Key::generate();

  const std::string &public_key = key.public_key();
  const std::string encoded = public_key.substr(0, public_key.find(' '));

  std::vector<unsigned char> blob(4 * ((524 + 2) / 3) + 1);
  std::size_t blob_size = 0;
  REQUIRE(mbedtls_base64_decode(
              blob.data(), blob.size(), &blob_size,
              reinterpret_cast<const unsigned char *>(encoded.data()),
              encoded.size()) == 0);
  REQUIRE(blob_size == 524);

  const auto read_u32 = [&blob](std::size_t offset) {
    return static_cast<std::uint32_t>(blob[offset]) |
           (static_cast<std::uint32_t>(blob[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(blob[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(blob[offset + 3]) << 24);
  };

  REQUIRE(read_u32(0) == 64);

  const std::uint32_t n0 = read_u32(8);
  const std::uint32_t n0inv = read_u32(4);
  REQUIRE(n0 * n0inv == 0xFFFFFFFFu);

  std::array<unsigned char, 256> modulus{};
  std::copy(blob.begin() + 8, blob.begin() + 8 + 256, modulus.begin());
  std::reverse(modulus.begin(), modulus.end());

  std::array<unsigned char, 256> rr_bytes{};
  std::copy(blob.begin() + 8 + 256, blob.begin() + 8 + 512, rr_bytes.begin());
  std::reverse(rr_bytes.begin(), rr_bytes.end());

  mbedtls_mpi n;
  mbedtls_mpi rr;
  mbedtls_mpi expected;
  mbedtls_mpi r;
  mbedtls_mpi_init(&n);
  mbedtls_mpi_init(&rr);
  mbedtls_mpi_init(&expected);
  mbedtls_mpi_init(&r);
  REQUIRE(mbedtls_mpi_read_binary(&n, modulus.data(), modulus.size()) == 0);
  REQUIRE(mbedtls_mpi_read_binary(&rr, rr_bytes.data(), rr_bytes.size()) == 0);

  REQUIRE(mbedtls_mpi_lset(&r, 1) == 0);
  REQUIRE(mbedtls_mpi_shift_l(&r, 2048) == 0);
  REQUIRE(mbedtls_mpi_mul_mpi(&expected, &r, &r) == 0);
  REQUIRE(mbedtls_mpi_mod_mpi(&expected, &expected, &n) == 0);
  REQUIRE(mbedtls_mpi_cmp_mpi(&rr, &expected) == 0);

  mbedtls_mpi_free(&r);
  mbedtls_mpi_free(&expected);
  mbedtls_mpi_free(&rr);
  mbedtls_mpi_free(&n);
}
