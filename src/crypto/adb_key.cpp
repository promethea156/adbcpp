#include "adbcpp/crypto/adb_key.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <mbedtls/base64.h>
#include <mbedtls/bignum.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/rsa.h>
#include <mbedtls/sha1.h>

namespace adbcpp::crypto {
namespace {

constexpr std::size_t kModulusBytes = 256;
constexpr std::size_t kHalfModulusBytes = kModulusBytes / 2;
constexpr std::size_t kBlobSize = 4 + 4 + kModulusBytes + kModulusBytes + 4;
constexpr std::size_t kPrivateKeySize =
    kBlobSize + kModulusBytes + 5 * kHalfModulusBytes;

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

void from_little_endian(mbedtls_mpi &value, std::span<const std::byte> in) {
  std::vector<unsigned char> big_endian(in.size(), 0);
  for (std::size_t i = 0; i < in.size(); ++i) {
    big_endian[i] = std::to_integer<std::uint8_t>(in[in.size() - 1 - i]);
  }
  if (mbedtls_mpi_read_binary(&value, big_endian.data(), big_endian.size()) !=
      0) {
    throw std::runtime_error("adbcpp: failed to parse an RSA component");
  }
}

// Computes rr = R^2 mod n, where R = 2^(8 * kModulusBytes).
mbedtls_mpi compute_rr(const mbedtls_mpi &n) {
  mbedtls_mpi rr;
  mbedtls_mpi_init(&rr);
  if (mbedtls_mpi_lset(&rr, 1) != 0 ||
      mbedtls_mpi_shift_l(&rr, 8 * kModulusBytes * 2) != 0 ||
      mbedtls_mpi_mod_mpi(&rr, &rr, &n) != 0) {
    mbedtls_mpi_free(&rr);
    throw std::runtime_error("adbcpp: failed to derive RSA key parameters");
  }
  return rr;
}

std::string encode_public_key(const mbedtls_mpi &n, const mbedtls_mpi &rr,
                              const mbedtls_mpi &e) {
  std::array<std::byte, kBlobSize> blob{};
  auto modulus = std::span(blob).subspan(8, kModulusBytes);
  to_little_endian(n, modulus);
  to_little_endian(rr, std::span(blob).subspan(8 + kModulusBytes, kModulusBytes));
  to_little_endian(e, std::span(blob).subspan(8 + 2 * kModulusBytes, 4));

  const std::uint32_t n0 = read_u32_le(modulus.data());
  write_u32_le(blob.data() + 0, static_cast<std::uint32_t>(kModulusBytes / 4));
  write_u32_le(blob.data() + 4, 0u - inverse_mod_2_32(n0));

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
  mbedtls_rsa_context rsa;
  std::string public_key;

  Impl() { mbedtls_rsa_init(&rsa); }
  ~Impl() { mbedtls_rsa_free(&rsa); }
};

Key::Key() : impl_(std::make_unique<Impl>()) {}
Key::~Key() = default;
Key::Key(Key &&) noexcept = default;
Key &Key::operator=(Key &&) noexcept = default;

const std::string &Key::public_key() const noexcept {
  return impl_->public_key;
}

Key Key::generate() {
  Key key;

  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&drbg);

  int rc =
      mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, nullptr, 0);
  if (rc == 0) {
    rc = mbedtls_rsa_gen_key(&key.impl_->rsa, mbedtls_ctr_drbg_random, &drbg,
                              2048, 65537);
  }
  mbedtls_ctr_drbg_free(&drbg);
  mbedtls_entropy_free(&entropy);
  if (rc != 0) {
    throw std::runtime_error("adbcpp: failed to generate an RSA key");
  }

  mbedtls_mpi n;
  mbedtls_mpi e;
  mbedtls_mpi_init(&n);
  mbedtls_mpi_init(&e);
  if (mbedtls_rsa_export(&key.impl_->rsa, &n, nullptr, nullptr, nullptr, &e) !=
      0) {
    mbedtls_mpi_free(&e);
    mbedtls_mpi_free(&n);
    throw std::runtime_error("adbcpp: failed to export an RSA key");
  }
  auto rr = compute_rr(n);
  key.impl_->public_key = encode_public_key(n, rr, e);
  mbedtls_mpi_free(&rr);
  mbedtls_mpi_free(&e);
  mbedtls_mpi_free(&n);
  return key;
}

Key Key::load_or_generate() {
  const auto directory = key_directory();
  const auto private_path = directory / "adbkey";
  const auto public_path = directory / "adbkey.pub";

  if (std::filesystem::exists(private_path)) {
    std::ifstream input(private_path, std::ios::binary);
    std::array<std::byte, kPrivateKeySize> blob{};
    input.read(reinterpret_cast<char *>(blob.data()),
               static_cast<std::streamsize>(blob.size()));
    if (input.gcount() != static_cast<std::streamsize>(blob.size())) {
      throw std::runtime_error("adbcpp: failed to read the ADB private key");
    }

    mbedtls_mpi n;
    mbedtls_mpi e;
    mbedtls_mpi d;
    mbedtls_mpi p;
    mbedtls_mpi q;
    mbedtls_mpi_init(&n);
    mbedtls_mpi_init(&e);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&p);
    mbedtls_mpi_init(&q);
    from_little_endian(n, std::span(blob).subspan(8, kModulusBytes));
    from_little_endian(e, std::span(blob).subspan(8 + 2 * kModulusBytes, 4));
    from_little_endian(d, std::span(blob).subspan(kBlobSize, kModulusBytes));
    from_little_endian(
        p, std::span(blob).subspan(kBlobSize + kModulusBytes, kHalfModulusBytes));
    from_little_endian(q, std::span(blob).subspan(
                             kBlobSize + kModulusBytes + kHalfModulusBytes,
                             kHalfModulusBytes));

    Key key;
    int rc = mbedtls_rsa_import(&key.impl_->rsa, &n, &p, &q, &d, &e);
    if (rc == 0) {
      rc = mbedtls_rsa_complete(&key.impl_->rsa);
    }
    if (rc != 0) {
      mbedtls_mpi_free(&q);
      mbedtls_mpi_free(&p);
      mbedtls_mpi_free(&d);
      mbedtls_mpi_free(&e);
      mbedtls_mpi_free(&n);
      throw std::runtime_error("adbcpp: failed to load the ADB private key");
    }

    auto rr = compute_rr(n);
    key.impl_->public_key = encode_public_key(n, rr, e);
    mbedtls_mpi_free(&rr);
    mbedtls_mpi_free(&q);
    mbedtls_mpi_free(&p);
    mbedtls_mpi_free(&d);
    mbedtls_mpi_free(&e);
    mbedtls_mpi_free(&n);
    return key;
  }

  Key key = generate();
  std::filesystem::create_directories(directory);

  mbedtls_mpi n;
  mbedtls_mpi e;
  mbedtls_mpi d;
  mbedtls_mpi p;
  mbedtls_mpi q;
  mbedtls_mpi_init(&n);
  mbedtls_mpi_init(&e);
  mbedtls_mpi_init(&d);
  mbedtls_mpi_init(&p);
  mbedtls_mpi_init(&q);
  if (mbedtls_rsa_export(&key.impl_->rsa, &n, &p, &q, &d, &e) != 0) {
    mbedtls_mpi_free(&q);
    mbedtls_mpi_free(&p);
    mbedtls_mpi_free(&d);
    mbedtls_mpi_free(&e);
    mbedtls_mpi_free(&n);
    throw std::runtime_error("adbcpp: failed to export the RSA key");
  }

  std::array<std::byte, kPrivateKeySize> blob{};
  auto modulus = std::span(blob).subspan(8, kModulusBytes);
  to_little_endian(n, modulus);
  auto rr = compute_rr(n);
  to_little_endian(rr,
                   std::span(blob).subspan(8 + kModulusBytes, kModulusBytes));
  to_little_endian(e, std::span(blob).subspan(8 + 2 * kModulusBytes, 4));
  const std::uint32_t n0 = read_u32_le(modulus.data());
  write_u32_le(blob.data() + 0, static_cast<std::uint32_t>(kModulusBytes / 4));
  write_u32_le(blob.data() + 4, 0u - inverse_mod_2_32(n0));

  std::size_t offset = kBlobSize;
  to_little_endian(d, std::span(blob).subspan(offset, kModulusBytes));
  offset += kModulusBytes;
  to_little_endian(p, std::span(blob).subspan(offset, kHalfModulusBytes));
  offset += kHalfModulusBytes;
  to_little_endian(q, std::span(blob).subspan(offset, kHalfModulusBytes));
  offset += kHalfModulusBytes;

  mbedtls_mpi pm1;
  mbedtls_mpi qm1;
  mbedtls_mpi dp;
  mbedtls_mpi dq;
  mbedtls_mpi qinv;
  mbedtls_mpi_init(&pm1);
  mbedtls_mpi_init(&qm1);
  mbedtls_mpi_init(&dp);
  mbedtls_mpi_init(&dq);
  mbedtls_mpi_init(&qinv);
  const bool derived =
      mbedtls_mpi_sub_int(&pm1, &p, 1) == 0 &&
      mbedtls_mpi_mod_mpi(&dp, &d, &pm1) == 0 &&
      mbedtls_mpi_sub_int(&qm1, &q, 1) == 0 &&
      mbedtls_mpi_mod_mpi(&dq, &d, &qm1) == 0 &&
      mbedtls_mpi_inv_mod(&qinv, &q, &p) == 0;
  if (derived) {
    to_little_endian(dp, std::span(blob).subspan(offset, kHalfModulusBytes));
    offset += kHalfModulusBytes;
    to_little_endian(dq, std::span(blob).subspan(offset, kHalfModulusBytes));
    offset += kHalfModulusBytes;
    to_little_endian(qinv, std::span(blob).subspan(offset, kHalfModulusBytes));
  }
  mbedtls_mpi_free(&qinv);
  mbedtls_mpi_free(&dq);
  mbedtls_mpi_free(&dp);
  mbedtls_mpi_free(&qm1);
  mbedtls_mpi_free(&pm1);
  mbedtls_mpi_free(&rr);
  mbedtls_mpi_free(&q);
  mbedtls_mpi_free(&p);
  mbedtls_mpi_free(&d);
  mbedtls_mpi_free(&e);
  mbedtls_mpi_free(&n);
  if (!derived) {
    throw std::runtime_error("adbcpp: failed to derive the RSA key");
  }

  std::ofstream private_output(private_path, std::ios::binary | std::ios::trunc);
  private_output.write(reinterpret_cast<const char *>(blob.data()),
                       static_cast<std::streamsize>(blob.size()));

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
  const int seed = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                         nullptr, 0);

  std::vector<std::byte> signature(mbedtls_rsa_get_len(&impl_->rsa));
  int rc = seed;
  if (rc == 0) {
    rc = mbedtls_rsa_pkcs1_sign(
        &impl_->rsa, mbedtls_ctr_drbg_random, &drbg, MBEDTLS_MD_SHA1,
        hash.size(), hash.data(),
        reinterpret_cast<unsigned char *>(signature.data()));
  }
  mbedtls_ctr_drbg_free(&drbg);
  mbedtls_entropy_free(&entropy);
  if (rc != 0) {
    throw std::runtime_error("adbcpp: failed to sign the token (" +
                            std::to_string(rc) + ")");
  }
  return signature;
}

} // namespace adbcpp::crypto
