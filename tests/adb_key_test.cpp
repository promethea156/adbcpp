#include <string>

#include <catch2/catch_test_macros.hpp>

#include "adbcpp/crypto/adb_key.hpp"

TEST_CASE("generated public key has the ADB format", "[crypto]") {
  const std::string key = adbcpp::crypto::generate_public_key();

  const auto space = key.find(' ');
  REQUIRE(space != std::string::npos);
  // 524-byte RSAPublicKey structure, base64-encoded.
  REQUIRE(space == 700);
  REQUIRE(key.substr(space) == " adbcpp@localhost");
}

TEST_CASE("generated public keys are unique", "[crypto]") {
  REQUIRE(adbcpp::crypto::generate_public_key() !=
          adbcpp::crypto::generate_public_key());
}
