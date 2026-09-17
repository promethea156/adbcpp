#include "adbcpp/crypto/adb_key.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <mbedtls/base64.h>
#include <mbedtls/bignum.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/md5.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/sha1.h>

namespace adbcpp::crypto {
namespace {

constexpr std::size_t kModulusBytes = 256;
constexpr std::size_t kBlobSize = 4 + 4 + kModulusBytes + kModulusBytes + 4;
constexpr std::size_t kPemBufferSize = 8192;

void write_u32_le(std::byte *out, std::uint32_t value) {
  out[0] = static_cast<std::byte>(value & 0xFFu);
  out[1] = static_cast<std::byte>((value >> 8) & 0xFFu);
  out[2] = static_cast<std::byte>((value >> 16) & 0xFFu);
  out[3] = static_cast<std::byte>((value >> 24) & 0xFFu);
}

std::uint32_t read_u32_le(const std::byte *in) {
  return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[0])) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[1])) << 8) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[2])) << 16) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[3])) << 24);
}

// Returns the multiplicative inverse of `value` modulo 2^32.
std::uint32_t inverse_mod_2_32(std::uint32_t value) {
  std::uint32_t inverse = 1;
  for (int i = 0; i < 5; ++i) {
    inverse *= 2u - value * inverse;
  }
  return inverse;
}

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

// Computes rr = R^2 mod n, where R = 2^(8 * kModulusBytes).
void compute_rr(mbedtls_mpi &rr, const mbedtls_mpi &n) {
  if (mbedtls_mpi_lset(&rr, 1) != 0 ||
      mbedtls_mpi_shift_l(&rr, 8 * kModulusBytes * 2) != 0 ||
      mbedtls_mpi_mod_mpi(&rr, &rr, &n) != 0) {
    throw std::runtime_error("adbcpp: failed to derive RSA key parameters");
  }
}

std::string encode_public_key(const mbedtls_pk_context &pk) {
  mbedtls_mpi n;
  mbedtls_mpi e;
  mbedtls_mpi rr;
  mbedtls_mpi_init(&n);
  mbedtls_mpi_init(&e);
  mbedtls_mpi_init(&rr);
  if (mbedtls_rsa_export(mbedtls_pk_rsa(pk), &n, nullptr, nullptr, nullptr,
                           &e) != 0) {
    mbedtls_mpi_free(&rr);
    mbedtls_mpi_free(&e);
    mbedtls_mpi_free(&n);
    throw std::runtime_error("adbcpp: failed to export the RSA key");
  }
  compute_rr(rr, n);

  std::array<std::byte, kBlobSize> blob{};
  auto modulus = std::span(blob).subspan(8, kModulusBytes);
  to_little_endian(n, modulus);
  to_little_endian(rr, std::span(blob).subspan(8 + kModulusBytes, kModulusBytes));
  to_little_endian(e, std::span(blob).subspan(8 + 2 * kModulusBytes, 4));

  const std::uint32_t n0 = read_u32_le(modulus.data());
  write_u32_le(blob.data() + 0, static_cast<std::uint32_t>(kModulusBytes / 4));
  write_u32_le(blob.data() + 4, 0u - inverse_mod_2_32(n0));

  mbedtls_mpi_free(&rr);
  mbedtls_mpi_free(&e);
  mbedtls_mpi_free(&n);

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

std::filesystem::path key_directory() {
#if defined(_WIN32)
  const char *home = std::getenv("USERPROFILE");
#else
  const char *home = std::getenv("HOME");
#endif
  if (home == nullptr || *home == '\0') {
    throw std::runtime_error("adbcpp: cannot locate the home directory");
  }
  return std::filesystem::path(home) / ".android";
}

} // namespace

struct Key::Impl {
  mbedtls_pk_context pk;
  std::string public_key;

  Impl() { mbedtls_pk_init(&pk); }
  ~Impl() { mbedtls_pk_free(&pk); }
};

Key::Key() : impl_(std::make_unique<Impl>()) {}
Key::~Key() = default;
Key::Key(Key &&) noexcept = default;
Key &Key::operator=(Key &&) noexcept = default;

const std::string &Key::public_key() const noexcept {
  return impl_->public_key;
}

std::string Key::fingerprint() const {
  const std::string encoded =
      impl_->public_key.substr(0, impl_->public_key.find(' '));

  std::vector<unsigned char> blob(4 * ((kBlobSize + 2) / 3) + 1);
  std::size_t blob_size = 0;
  if (mbedtls_base64_decode(
          blob.data(), blob.size(), &blob_size,
          reinterpret_cast<const unsigned char *>(encoded.data()),
          encoded.size()) != 0) {
    throw std::runtime_error("adbcpp: failed to decode the ADB public key");
  }

  std::array<unsigned char, 16> digest{};
  mbedtls_md5(blob.data(), blob_size, digest.data());

  constexpr char kHex[] = "0123456789ABCDEF";
  std::string result;
  result.reserve(digest.size() * 3);
  for (std::size_t i = 0; i < digest.size(); ++i) {
    if (i != 0) {
      result.push_back(':');
    }
    result.push_back(kHex[digest[i] >> 4]);
    result.push_back(kHex[digest[i] & 0x0F]);
  }
  return result;
}

Key Key::generate() {
  Key key;
  if (mbedtls_pk_setup(&key.impl_->pk,
                        mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) != 0) {
    throw std::runtime_error("adbcpp: failed to set up an RSA key");
  }

  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&drbg);

  int rc =
      mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, nullptr, 0);
  if (rc == 0) {
    rc = mbedtls_rsa_gen_key(mbedtls_pk_rsa(key.impl_->pk),
                              mbedtls_ctr_drbg_random, &drbg, 2048, 65537);
  }
  mbedtls_ctr_drbg_free(&drbg);
  mbedtls_entropy_free(&entropy);
  if (rc != 0) {
    throw std::runtime_error("adbcpp: failed to generate an RSA key");
  }

  key.impl_->public_key = encode_public_key(key.impl_->pk);
  return key;
}

Key Key::load_or_generate() {
  const auto directory = key_directory();
  const auto private_path = directory / "adbkey";
  const auto public_path = directory / "adbkey.pub";

  if (std::filesystem::exists(private_path)) {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    const int seed =
        mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, nullptr, 0);

    Key key;
    int rc = seed;
    if (rc == 0) {
      rc = mbedtls_pk_parse_keyfile(
          &key.impl_->pk, private_path.string().c_str(), nullptr,
          mbedtls_ctr_drbg_random, &drbg);
    }
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    if (rc != 0) {
      throw std::runtime_error("adbcpp: failed to load the ADB private key (" +
                              std::to_string(rc) + ")");
    }

    key.impl_->public_key = encode_public_key(key.impl_->pk);
    return key;
  }

  Key key = generate();
  std::filesystem::create_directories(directory);

  std::array<unsigned char, kPemBufferSize> pem{};
  const int length = mbedtls_pk_write_key_pem(&key.impl_->pk, pem.data(),
                                              pem.size());
  if (length == 0) {
    throw std::runtime_error("adbcpp: failed to store the ADB private key");
  }
  std::ofstream private_output(private_path, std::ios::binary | std::ios::trunc);
  private_output.write(reinterpret_cast<const char *>(pem.data()), length);

  std::ofstream public_output(public_path, std::ios::binary | std::ios::trunc);
  if (public_output) {
    public_output << key.public_key() << '\n';
  }
  return key;
}

std::vector<std::byte> Key::sign(std::span<const std::byte> token) const {
  std::array<unsigned char, 20> hash{};
  mbedtls_sha1(reinterpret_cast<const unsigned char *>(token.data()),
                token.size(), hash.data());

  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&drbg);
  const int seed =
      mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, nullptr, 0);

  std::vector<unsigned char> signature(mbedtls_pk_get_len(&impl_->pk));
  std::size_t length = 0;
  int rc = seed;
  if (rc == 0) {
    rc = mbedtls_pk_sign(&impl_->pk, MBEDTLS_MD_SHA1, hash.data(), hash.size(),
                          signature.data(), signature.size(), &length,
                          mbedtls_ctr_drbg_random, &drbg);
  }
  mbedtls_ctr_drbg_free(&drbg);
  mbedtls_entropy_free(&entropy);
  if (rc != 0) {
    throw std::runtime_error("adbcpp: failed to sign the token (" +
                            std::to_string(rc) + ")");
  }
  signature.resize(length);

  std::vector<std::byte> result(signature.size());
  for (std::size_t i = 0; i < signature.size(); ++i) {
    result[i] = static_cast<std::byte>(signature[i]);
  }
  return result;
}

} // namespace adbcpp::crypto
