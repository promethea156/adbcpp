#include "adbcpp/protocol/message.hpp"

namespace adbcpp::protocol
{
namespace
{

// Every header field is a 32-bit little-endian word on the wire. These helpers
// keep the byte order explicit instead of relying on the host's endianness.
void write_u32_le(std::byte *out, std::uint32_t value) noexcept
{
    out[0] = static_cast<std::byte>(value & 0xFFu);
    out[1] = static_cast<std::byte>((value >> 8) & 0xFFu);
    out[2] = static_cast<std::byte>((value >> 16) & 0xFFu);
    out[3] = static_cast<std::byte>((value >> 24) & 0xFFu);
}

std::uint32_t read_u32_le(const std::byte *in) noexcept
{
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[0])) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[1])) << 8) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[2])) << 16) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[3])) << 24);
}

} // namespace

std::uint32_t Message::compute_magic(std::uint32_t command) noexcept
{
    return command ^ 0xFFFFFFFFu;
}

std::uint32_t Message::compute_crc32(std::span<const std::byte> data) noexcept
{
    // Reflected CRC-32 with the zlib polynomial 0xEDB88320 (the reverse of the
    // 0x04C11DB7 polynomial), seeded and finalized with 0xFFFFFFFF. The `~` at the
    // end is the final XOR, which is what makes this the "standard" CRC32 that
    // `adb` and `zlib` produce.
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const std::byte value : data)
    {
        crc ^= std::to_integer<std::uint8_t>(value);
        for (int bit = 0; bit < 8; ++bit)
        {
            const std::uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

Result<std::uint32_t> Message::data_length_of(std::span<const std::byte> payload)
{
    if (payload.size() > 0xFFFFFFFFu)
    {
        return tl::unexpected(Error{ErrorCode::InvalidArgument, "the payload is too large for the ADB header"});
    }
    return static_cast<std::uint32_t>(payload.size());
}

std::array<std::byte, kMessageHeaderSize> Message::encode() const noexcept
{
    std::array<std::byte, kMessageHeaderSize> bytes{};
    write_u32_le(bytes.data() + 0, command);
    write_u32_le(bytes.data() + 4, arg0);
    write_u32_le(bytes.data() + 8, arg1);
    write_u32_le(bytes.data() + 12, data_length);
    write_u32_le(bytes.data() + 16, data_crc32);
    write_u32_le(bytes.data() + 20, magic);
    return bytes;
}

Message Message::decode(std::span<const std::byte, kMessageHeaderSize> bytes) noexcept
{
    Message message;
    message.command = read_u32_le(bytes.data() + 0);
    message.arg0 = read_u32_le(bytes.data() + 4);
    message.arg1 = read_u32_le(bytes.data() + 8);
    message.data_length = read_u32_le(bytes.data() + 12);
    message.data_crc32 = read_u32_le(bytes.data() + 16);
    message.magic = read_u32_le(bytes.data() + 20);
    return message;
}

} // namespace adbcpp::protocol
