#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "adbcpp/adbcpp.hpp"
#include "adbcpp/sync.hpp"
#include "adbcpp/testing/mock_transport.hpp"

// A `list` round-trip with no device attached. The mock transport queues the
// bytes a device would send, so the real `list` code runs end to end and both
// halves of the exchange are visible. The protocol is explained in
// `docs/06-sync-protocol.md`.
namespace
{

// A stream is keyed by two ids, and the ids are relative to the sender, so each
// side's local id is the other's remote id. These are the values a real device
// uses for the first stream after the connection.
constexpr std::uint32_t kDeviceId = 7;
constexpr std::uint32_t kLocalId = 2;

// Every binary integer in sync mode is little-endian, like the ADB header.
void write_u32_le(std::byte *out, std::uint32_t value)
{
    out[0] = static_cast<std::byte>(value & 0xFFu);
    out[1] = static_cast<std::byte>((value >> 8) & 0xFFu);
    out[2] = static_cast<std::byte>((value >> 16) & 0xFFu);
    out[3] = static_cast<std::byte>((value >> 24) & 0xFFu);
}

void write_u64_le(std::byte *out, std::uint64_t value)
{
    write_u32_le(out, static_cast<std::uint32_t>(value));
    write_u32_le(out + 4, static_cast<std::uint32_t>(value >> 32));
}

std::span<const std::byte> bytes_of(const std::string &text)
{
    return std::span(reinterpret_cast<const std::byte *>(text.data()), text.size());
}

// An ADB header with its derived fields (`data_length`, `data_crc32`, `magic`)
// filled in.
adbcpp::protocol::Message message(std::uint32_t command, std::uint32_t arg0, std::uint32_t arg1,
                                  std::span<const std::byte> payload = {})
{
    adbcpp::protocol::Message header;
    header.command = command;
    header.arg0 = arg0;
    header.arg1 = arg1;
    header.data_length = adbcpp::protocol::Message::data_length_of(payload);
    header.data_crc32 = adbcpp::protocol::Message::compute_crc32(payload);
    header.magic = adbcpp::protocol::Message::compute_magic(command);
    return header;
}

void feed(adbcpp::testing::MockTransport &transport, const adbcpp::protocol::Message &header,
          std::span<const std::byte> payload = {})
{
    transport.feed(header.encode());
    if (!payload.empty())
    {
        transport.feed(payload);
    }
}

// A sync id is the little-endian encoding of four ASCII characters, exactly like
// an ADB command.
std::uint32_t sync_id(const char *four)
{
    return adbcpp::protocol::make_command(four[0], four[1], four[2], four[3]);
}

// A v2 DENT: id, error, dev, ino, mode, nlink, uid, gid, size, atime, mtime,
// ctime, namelen, name. The field offsets are the ones `list` parses.
std::vector<std::byte> dent_v2(std::uint32_t mode, std::uint64_t size, const std::string &name)
{
    std::vector<std::byte> dent(76 + name.size());
    write_u32_le(dent.data(), sync_id("DNT2"));
    write_u32_le(dent.data() + 4, 0u); // error
    write_u32_le(dent.data() + 24, mode);
    write_u64_le(dent.data() + 40, size);
    write_u32_le(dent.data() + 72, static_cast<std::uint32_t>(name.size()));
    std::copy_n(reinterpret_cast<const std::byte *>(name.data()), static_cast<std::ptrdiff_t>(name.size()),
                dent.begin() + 76);
    return dent;
}

// DONE is a full DENT struct whose id is DONE.
std::vector<std::byte> done_v2()
{
    std::vector<std::byte> done(76);
    write_u32_le(done.data(), sync_id("DONE"));
    return done;
}

// Print bytes the way a capture shows them.
void print_bytes(std::string_view label, std::span<const std::byte> data)
{
    std::cout << label << ':';
    for (const std::byte value : data)
    {
        std::cout << ' ' << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<int>(std::to_integer<std::uint8_t>(value));
    }
    std::cout << std::dec << '\n';
}

} // namespace

int main()
{
    adbcpp::testing::MockTransport transport;

    // 1. The device's CNXN. Its banner advertises `ls_v2`, so `list` chooses the
    //    v2 DNT2 entry form. `arg1` is the maximum payload.
    const std::string banner = "device::features=shell_v2,ls_v2";
    feed(transport, message(adbcpp::protocol::kCnxn, 0x01000001u, 4096u, bytes_of(banner)), bytes_of(banner));

    // 2. The device accepts our OPEN of `sync:` with an OKAY.
    feed(transport, message(adbcpp::protocol::kOkay, kDeviceId, kLocalId));

    // 3. The listing: two entries and DONE. A sync response is not an ADB
    //    message, so it travels inside the payload of a WRTE.
    for (const auto &dent : {dent_v2(0040755u, 4096u, "."), dent_v2(0100644u, 30u, "adb_keys")})
    {
        feed(transport, message(adbcpp::protocol::kWrte, kDeviceId, kLocalId, dent), dent);
    }
    const auto done = done_v2();
    feed(transport, message(adbcpp::protocol::kWrte, kDeviceId, kLocalId, done), done);

    // The connection performs the handshake against the queued CNXN.
    adbcpp::Connection connection(transport);

    std::cout << "listing /sdcard:\n";
    for (const auto &entry : adbcpp::list(connection, "/sdcard"))
    {
        std::cout << "  " << (entry.is_directory() ? 'd' : '-') << ' ' << entry.size << ' ' << entry.name << '\n';
    }

    // Show the LIS2 request that `list` wrote, to make the wire format concrete.
    // It sits in the byte stream after the CNXN and OPEN headers.
    const auto &written = transport.written();
    std::array<std::byte, 4> list_id{};
    write_u32_le(list_id.data(), sync_id("LIS2"));
    const auto found = std::search(written.begin(), written.end(), list_id.begin(), list_id.end());
    if (found != written.end())
    {
        const auto offset = static_cast<std::size_t>(std::distance(written.begin(), found));
        print_bytes("the host wrote LIS2", std::span(written).subspan(offset, 8 + 7));
    }

    return 0;
}
