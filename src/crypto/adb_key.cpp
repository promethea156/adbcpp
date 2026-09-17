#include "adbcpp/crypto/adb_key.hpp"

#include <mbedtls/base64.h>
#include <mbedtls/bignum.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/md5.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>

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

namespace adbcpp::crypto
{
namespace
{

constexpr std::size_t kModulusBytes = 256;
constexpr std::size_t kBlobSize = 4 + 4 + kModulusBytes + kModulusBytes + 4;
constexpr std::size_t kPemBufferSize = 8192;

// ADB's public key is a custom binary structure, not a DER
// `SubjectPublicKeyInfo`:
//
//   struct RSAPublicKey {
//       uint32_t modulus_size_words;   // 2048 / 32 = 64
//       uint32_t n0inv;               // -1 / n[0] mod 2^32
//       uint32_t modulus[64];         // n, little-endian words
//       uint32_t rr[64];             // R^2 mod n, little-endian words
//       uint32_t exponent;           // e, little-endian (65537)
//   };
//
// The whole structure is serialized little-endian and then base64-encoded, so a
// 2048-bit key is 524 bytes and 700 base64 characters. This is AOSP's
// `android_pubkey_encode`:
//
//   https://android.googlesource.com/platform/system/core/+/refs/heads/main/libcrypto_utils/android_pubkey.cpp
//
// `n0inv` and `rr` exist for fast Montgomery modular exponentiation on the
// device. AOSP's `android_pubkey_decode` actually ignores them and lets
// BoringSSL recompute them, but older adbd implementations read them, so they
// must be correct. Unit tests check both.

void write_u32_le(std::byte *out, std::uint32_t value)
{
    out[0] = static_cast<std::byte>(value & 0xFFu);
    out[1] = static_cast<std::byte>((value >> 8) & 0xFFu);
    out[2] = static_cast<std::byte>((value >> 16) & 0xFFu);
    out[3] = static_cast<std::byte>((value >> 24) & 0xFFu);
}

std::uint32_t read_u32_le(const std::byte *in)
{
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[0])) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[1])) << 8) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[2])) << 16) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[3])) << 24);
}

// Returns the multiplicative inverse of `value` modulo 2^32.
//
// This is the Newton-Raphson iteration for the inverse modulo a power of two:
// starting from 1 (correct mod 2), each step doubles the number of correct bits,
// so five steps are enough for 32 bits. All arithmetic relies on unsigned
// wraparound.
std::uint32_t inverse_mod_2_32(std::uint32_t value)
{
    std::uint32_t inverse = 1;
    for (int i = 0; i < 5; ++i)
    {
        inverse *= 2u - value * inverse;
    }
    return inverse;
}

// Writes a bignum into `out` in little-endian byte order. mbedTLS serializes
// big-endian, which is the opposite of what ADB's key blob uses, so the result is
// reversed.
void to_little_endian(const mbedtls_mpi &value, std::span<std::byte> out)
{
    std::vector<unsigned char> big_endian(out.size(), 0);
    if (mbedtls_mpi_write_binary(&value, big_endian.data(), big_endian.size()) != 0)
    {
        throw std::runtime_error("adbcpp: failed to serialize an RSA component");
    }
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        out[i] = static_cast<std::byte>(big_endian[out.size() - 1 - i]);
    }
}

// Computes rr = R^2 mod n, where R = 2^(8 * kModulusBytes).
void compute_rr(mbedtls_mpi &rr, const mbedtls_mpi &n)
{
    if (mbedtls_mpi_lset(&rr, 1) != 0 || mbedtls_mpi_shift_l(&rr, 8 * kModulusBytes * 2) != 0 ||
        mbedtls_mpi_mod_mpi(&rr, &rr, &n) != 0)
    {
        throw std::runtime_error("adbcpp: failed to derive RSA key parameters");
    }
}

std::string encode_public_key(const mbedtls_pk_context &pk)
{
    mbedtls_mpi n;
    mbedtls_mpi e;
    mbedtls_mpi rr;
    mbedtls_mpi_init(&n);
    mbedtls_mpi_init(&e);
    mbedtls_mpi_init(&rr);
    if (mbedtls_rsa_export(mbedtls_pk_rsa(pk), &n, nullptr, nullptr, nullptr, &e) != 0)
    {
        mbedtls_mpi_free(&rr);
        mbedtls_mpi_free(&e);
        mbedtls_mpi_free(&n);
        throw std::runtime_error("adbcpp: failed to export the RSA key");
    }
    compute_rr(rr, n);

    // Lay the blob out exactly as `android_pubkey_encode` does: the word count,
    // then n0inv, the modulus, rr, and the exponent, all little-endian.
    std::array<std::byte, kBlobSize> blob{};
    auto modulus = std::span(blob).subspan(8, kModulusBytes);
    to_little_endian(n, modulus);
    to_little_endian(rr, std::span(blob).subspan(8 + kModulusBytes, kModulusBytes));
    to_little_endian(e, std::span(blob).subspan(8 + 2 * kModulusBytes, 4));

    // n0inv is -1 / n[0] mod 2^32, where n[0] is the least significant word
    // of the modulus. Unsigned negation is how the reference implementation
    // computes `-n0inv`.
    const std::uint32_t n0 = read_u32_le(modulus.data());
    write_u32_le(blob.data() + 0, static_cast<std::uint32_t>(kModulusBytes / 4));
    write_u32_le(blob.data() + 4, 0u - inverse_mod_2_32(n0));

    mbedtls_mpi_free(&rr);
    mbedtls_mpi_free(&e);
    mbedtls_mpi_free(&n);

    std::vector<unsigned char> encoded(4 * ((kBlobSize + 2) / 3) + 1, 0);
    std::size_t length = 0;
    if (mbedtls_base64_encode(encoded.data(), encoded.size(), &length,
                              reinterpret_cast<const unsigned char *>(blob.data()), blob.size()) != 0)
    {
        throw std::runtime_error("adbcpp: failed to encode the public key");
    }

    // AUTH type 3 sends "<base64 blob> <user>@<host>", and adbd splits on the
    // space to separate the key from its label. The label is free-form; it is only
    // shown in the device's authorization dialog.
    std::string key(reinterpret_cast<const char *>(encoded.data()), length);
    key += " adbcpp@localhost";
    return key;
}

// Returns the user's home directory, or throws if it is not set. On Windows this
// uses the secure `_dupenv_s`, which allocates the value and must be freed.
std::filesystem::path home_directory()
{
#if defined(_WIN32)
    char *value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, "USERPROFILE") != 0 || value == nullptr)
    {
        throw std::runtime_error("adbcpp: cannot locate the home directory");
    }
    const std::filesystem::path home(value);
    std::free(value);
    return home;
#else
    const char *value = std::getenv("HOME");
    if (value == nullptr || *value == '\0')
    {
        throw std::runtime_error("adbcpp: cannot locate the home directory");
    }
    return std::filesystem::path(value);
#endif
}

// ADB keeps its keys in `~/.android`, so adb and this library share the same
// key pair. Sharing the key is what avoids a new authorization prompt.
std::filesystem::path key_directory()
{
    return home_directory() / ".android";
}

} // namespace

struct Key::Impl
{
    mbedtls_pk_context pk;
    std::string public_key;

    Impl()
    {
        mbedtls_pk_init(&pk);
    }
    ~Impl()
    {
        mbedtls_pk_free(&pk);
    }
};

Key::Key()
    : impl_(std::make_unique<Impl>())
{
}
Key::~Key() = default;
Key::Key(Key &&) noexcept = default;
Key &Key::operator=(Key &&) noexcept = default;

const std::string &Key::public_key() const noexcept
{
    return impl_->public_key;
}

std::string Key::fingerprint() const
{
    const std::string encoded = impl_->public_key.substr(0, impl_->public_key.find(' '));

    std::vector<unsigned char> blob(4 * ((kBlobSize + 2) / 3) + 1);
    std::size_t blob_size = 0;
    if (mbedtls_base64_decode(blob.data(), blob.size(), &blob_size,
                              reinterpret_cast<const unsigned char *>(encoded.data()), encoded.size()) != 0)
    {
        throw std::runtime_error("adbcpp: failed to decode the ADB public key");
    }

    std::array<unsigned char, 16> digest{};
    mbedtls_md5(blob.data(), blob_size, digest.data());

    constexpr char kHex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(digest.size() * 3);
    for (std::size_t i = 0; i < digest.size(); ++i)
    {
        if (i != 0)
        {
            result.push_back(':');
        }
        result.push_back(kHex[digest[i] >> 4]);
        result.push_back(kHex[digest[i] & 0x0F]);
    }
    return result;
}

Key Key::generate()
{
    Key key;
    if (mbedtls_pk_setup(&key.impl_->pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) != 0)
    {
        throw std::runtime_error("adbcpp: failed to set up an RSA key");
    }

    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);

    int rc = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, nullptr, 0);
    if (rc == 0)
    {
        rc = mbedtls_rsa_gen_key(mbedtls_pk_rsa(key.impl_->pk), mbedtls_ctr_drbg_random, &drbg, 2048, 65537);
    }
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    if (rc != 0)
    {
        throw std::runtime_error("adbcpp: failed to generate an RSA key");
    }

    key.impl_->public_key = encode_public_key(key.impl_->pk);
    return key;
}

Key Key::load_or_generate()
{
    const auto directory = key_directory();
    const auto private_path = directory / "adbkey";
    const auto public_path = directory / "adbkey.pub";

    // Reuse adb's key if it is already there. `adbkey` is a PKCS#8 PEM, which
    // `mbedtls_pk_parse_keyfile` reads directly, so no manual ASN.1 parsing is
    // needed (blocker 8 in `04-blockers.md`).
    if (std::filesystem::exists(private_path))
    {
        mbedtls_entropy_context entropy;
        mbedtls_ctr_drbg_context drbg;
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
        const int seed = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, nullptr, 0);

        Key key;
        int rc = seed;
        if (rc == 0)
        {
            rc = mbedtls_pk_parse_keyfile(&key.impl_->pk, private_path.string().c_str(), nullptr,
                                          mbedtls_ctr_drbg_random, &drbg);
        }
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
        if (rc != 0)
        {
            throw std::runtime_error("adbcpp: failed to load the ADB private key (" + std::to_string(rc) + ")");
        }

        key.impl_->public_key = encode_public_key(key.impl_->pk);
        return key;
    }

    // No key yet: create `~/.android` and write both files in adb's format, so
    // that adb and this library can share the key afterwards.
    Key key = generate();
    std::filesystem::create_directories(directory);

    std::array<unsigned char, kPemBufferSize> pem{};
    const int length = mbedtls_pk_write_key_pem(&key.impl_->pk, pem.data(), pem.size());
    if (length == 0)
    {
        throw std::runtime_error("adbcpp: failed to store the ADB private key");
    }
    std::ofstream private_output(private_path, std::ios::binary | std::ios::trunc);
    private_output.write(reinterpret_cast<const char *>(pem.data()), length);

    std::ofstream public_output(public_path, std::ios::binary | std::ios::trunc);
    if (public_output)
    {
        public_output << key.public_key() << '\n';
    }
    return key;
}

std::vector<std::byte> Key::sign(std::span<const std::byte> token) const
{
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    const int seed = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, nullptr, 0);

    // The device's AUTH token is signed directly as the SHA-1 digest, exactly like
    // adb's RSA_sign(NID_sha1, token, token_size, ...) in `adb_auth_host.cpp`.
    // The token is 20 bytes because it is already the SHA-1 digest, so it must NOT
    // be hashed again: doing so was blocker 16 and made adbd reject every
    // signature and prompt for authorization on every run.
    //
    //   https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/adb_auth_host.cpp
    //
    // adbd verifies with the matching RSA_verify(NID_sha1, token, ...). PKCS#1 v1.5
    // signing is deterministic, so for the same token this is byte-for-byte what adb
    // produces.
    std::vector<unsigned char> signature(mbedtls_pk_get_len(&impl_->pk));
    std::size_t length = 0;
    int rc = seed;
    if (rc == 0)
    {
        rc = mbedtls_pk_sign(&impl_->pk, MBEDTLS_MD_SHA1, reinterpret_cast<const unsigned char *>(token.data()),
                             token.size(), signature.data(), signature.size(), &length, mbedtls_ctr_drbg_random, &drbg);
    }
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    if (rc != 0)
    {
        throw std::runtime_error("adbcpp: failed to sign the token (" + std::to_string(rc) + ")");
    }
    signature.resize(length);

    std::vector<std::byte> result(signature.size());
    for (std::size_t i = 0; i < signature.size(); ++i)
    {
        result[i] = static_cast<std::byte>(signature[i]);
    }
    return result;
}

} // namespace adbcpp::crypto
