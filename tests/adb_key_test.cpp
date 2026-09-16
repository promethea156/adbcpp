#include <array>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

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
