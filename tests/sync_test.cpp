#include "adbcpp/sync.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "adbcpp/connection.hpp"
#include "adbcpp/protocol/commands.hpp"
#include "adbcpp/protocol/message.hpp"
#include "adbcpp/testing/mock_transport.hpp"

using adbcpp::protocol::Message;

namespace
{

// Every binary integer in sync mode is little-endian.
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

adbcpp::protocol::Message make_message(std::uint32_t command, std::uint32_t arg0, std::uint32_t arg1,
                                       std::span<const std::byte> payload = {})
{
    adbcpp::protocol::Message message;
    message.command = command;
    message.arg0 = arg0;
    message.arg1 = arg1;
    message.data_length = adbcpp::protocol::Message::data_length_of(payload);
    message.data_crc32 = adbcpp::protocol::Message::compute_crc32(payload);
    message.magic = adbcpp::protocol::Message::compute_magic(command);
    return message;
}

void feed_frame(adbcpp::testing::MockTransport &transport, const adbcpp::protocol::Message &header,
                std::span<const std::byte> payload = {})
{
    transport.feed(header.encode());
    if (!payload.empty())
    {
        transport.feed(payload);
    }
}

// Feeds the device's CNXN with the given feature list, then the OKAY that
// accepts the stream's OPEN. The device's id for the stream is 7, ours is 2.
void feed_device(adbcpp::testing::MockTransport &transport, const std::string &features)
{
    const std::string banner = "device::features=" + features;
    feed_frame(transport, make_message(adbcpp::protocol::kCnxn, 0x01000001u, 4096u, bytes_of(banner)),
               bytes_of(banner));
    feed_frame(transport, make_message(adbcpp::protocol::kOkay, 7, 2));
}

// Feeds `data` inside a WRTE from the device on the stream. A sync response is
// not an ADB message, so it travels inside a WRTE payload, exactly like shell
// output. The ids are relative to the sender, so `arg0` is the device's id for the
// stream (7) and `arg1` is ours (2).
void feed_sync(adbcpp::testing::MockTransport &transport, std::span<const std::byte> data)
{
    feed_frame(transport, make_message(adbcpp::protocol::kWrte, 7, 2, data), data);
}

// Feeds one v1 DENT: id, mode, size, mtime, namelen, name.
void feed_dent_v1(adbcpp::testing::MockTransport &transport, std::uint32_t mode, std::uint32_t size,
                  std::uint32_t mtime, const std::string &name)
{
    std::vector<std::byte> dent(20 + name.size());
    write_u32_le(dent.data() + 0, adbcpp::protocol::make_command('D', 'E', 'N', 'T'));
    write_u32_le(dent.data() + 4, mode);
    write_u32_le(dent.data() + 8, size);
    write_u32_le(dent.data() + 12, mtime);
    write_u32_le(dent.data() + 16, static_cast<std::uint32_t>(name.size()));
    std::copy_n(reinterpret_cast<const std::byte *>(name.data()), static_cast<std::ptrdiff_t>(name.size()),
                dent.begin() + static_cast<std::ptrdiff_t>(20));
    feed_sync(transport, dent);
}

// Feeds one v2 DENT: id, error, dev, ino, mode, nlink, uid, gid, size, atime,
// mtime, ctime, namelen, name.
void feed_dent_v2(adbcpp::testing::MockTransport &transport, std::uint32_t error, std::uint32_t mode,
                  std::uint64_t size, std::int64_t mtime, const std::string &name)
{
    std::vector<std::byte> dent(76 + name.size());
    write_u32_le(dent.data() + 0, adbcpp::protocol::make_command('D', 'N', 'T', '2'));
    write_u32_le(dent.data() + 4, error);
    write_u64_le(dent.data() + 8, 0);  // dev
    write_u64_le(dent.data() + 16, 0); // ino
    write_u32_le(dent.data() + 24, mode);
    write_u32_le(dent.data() + 28, 1); // nlink
    write_u32_le(dent.data() + 32, 0); // uid
    write_u32_le(dent.data() + 36, 0); // gid
    write_u64_le(dent.data() + 40, size);
    write_u64_le(dent.data() + 48, 0); // atime
    write_u64_le(dent.data() + 56, static_cast<std::uint64_t>(mtime));
    write_u64_le(dent.data() + 64, 0); // ctime
    write_u32_le(dent.data() + 72, static_cast<std::uint32_t>(name.size()));
    std::copy_n(reinterpret_cast<const std::byte *>(name.data()), static_cast<std::ptrdiff_t>(name.size()),
                dent.begin() + static_cast<std::ptrdiff_t>(76));
    feed_sync(transport, dent);
}

// DONE is a full DENT struct with its id set to DONE.
void feed_done(adbcpp::testing::MockTransport &transport, bool v2)
{
    const std::size_t size = v2 ? 76 : 20;
    std::vector<std::byte> done(size);
    write_u32_le(done.data(), adbcpp::protocol::make_command('D', 'O', 'N', 'E'));
    feed_sync(transport, done);
}

void feed_fail(adbcpp::testing::MockTransport &transport, const std::string &reason)
{
    std::vector<std::byte> fail(8 + reason.size());
    write_u32_le(fail.data(), adbcpp::protocol::make_command('F', 'A', 'I', 'L'));
    write_u32_le(fail.data() + 4, static_cast<std::uint32_t>(reason.size()));
    std::copy_n(reinterpret_cast<const std::byte *>(reason.data()), static_cast<std::ptrdiff_t>(reason.size()),
                fail.begin() + static_cast<std::ptrdiff_t>(8));
    feed_sync(transport, fail);
}

} // namespace

TEST_CASE("list parses the v1 DENT entries", "[sync]")
{
    adbcpp::testing::MockTransport transport;
    feed_device(transport, "shell_v2,cmd");
    feed_dent_v1(transport, 0040755u, 0u, 123u, ".");
    feed_dent_v1(transport, 0100644u, 42u, 456u, "file.txt");
    feed_done(transport, false);

    adbcpp::Connection connection(transport);
    const auto entries = adbcpp::list(connection, "/sdcard");

    REQUIRE(entries.size() == 2);
    REQUIRE(entries[0].name == ".");
    REQUIRE(entries[0].is_directory());
    REQUIRE(entries[1].name == "file.txt");
    REQUIRE(entries[1].is_regular());
    REQUIRE(entries[1].size == 42u);
    REQUIRE(entries[1].mtime == 456);
}

TEST_CASE("list parses the v2 DNT2 entries", "[sync]")
{
    adbcpp::testing::MockTransport transport;
    feed_device(transport, "shell_v2,ls_v2");
    feed_dent_v2(transport, 0u, 0040755u, 4096u, 789, "sdcard");
    feed_done(transport, true);

    adbcpp::Connection connection(transport);
    const auto entries = adbcpp::list(connection, "/");

    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].name == "sdcard");
    REQUIRE(entries[0].is_directory());
    REQUIRE(entries[0].size == 4096u);
    REQUIRE(entries[0].mtime == 789);
}

TEST_CASE("list sends the LIST request with the v2 id when ls_v2 is advertised", "[sync]")
{
    adbcpp::testing::MockTransport transport;
    feed_device(transport, "shell_v2,ls_v2");
    feed_done(transport, true);

    adbcpp::Connection connection(transport);
    adbcpp::list(connection, "/sdcard");

    // LIST(id, path_length, "path"), with no null terminator.
    std::vector<std::byte> request(8 + 7);
    write_u32_le(request.data(), adbcpp::protocol::make_command('L', 'I', 'S', '2'));
    write_u32_le(request.data() + 4, 7u);
    std::copy_n(reinterpret_cast<const std::byte *>("/sdcard"), 7, request.begin() + static_cast<std::ptrdiff_t>(8));

    const auto &written = transport.written();
    REQUIRE(std::search(written.begin(), written.end(), request.begin(), request.end()) != written.end());
}

TEST_CASE("list leaves sync mode with QUIT", "[sync]")
{
    adbcpp::testing::MockTransport transport;
    feed_device(transport, "shell_v2,cmd");
    feed_done(transport, false);

    adbcpp::Connection connection(transport);
    adbcpp::list(connection, "/");

    std::vector<std::byte> quit(8);
    write_u32_le(quit.data(), adbcpp::protocol::make_command('Q', 'U', 'I', 'T'));
    write_u32_le(quit.data() + 4, 0u);

    const auto &written = transport.written();
    REQUIRE(std::search(written.begin(), written.end(), quit.begin(), quit.end()) != written.end());
}

TEST_CASE("list skips a v2 entry that failed to stat", "[sync]")
{
    adbcpp::testing::MockTransport transport;
    feed_device(transport, "shell_v2,ls_v2");
    // EACCES for an entry whose name is still sent.
    feed_dent_v2(transport, 13u, 0u, 0u, 0, "secret");
    feed_dent_v2(transport, 0u, 0100644u, 10u, 1, "file.txt");
    feed_done(transport, true);

    adbcpp::Connection connection(transport);
    const auto entries = adbcpp::list(connection, "/");

    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].name == "file.txt");
    REQUIRE(entries[0].size == 10u);
}

TEST_CASE("list rejects a path longer than the sync limit", "[sync]")
{
    adbcpp::testing::MockTransport transport;
    feed_device(transport, "shell_v2,cmd");

    adbcpp::Connection connection(transport);
    const std::string path(1025, 'a');

    REQUIRE_THROWS(adbcpp::list(connection, path));
}

TEST_CASE("list throws when the sync request fails", "[sync]")
{
    adbcpp::testing::MockTransport transport;
    feed_device(transport, "shell_v2,cmd");
    feed_fail(transport, "path too long");

    adbcpp::Connection connection(transport);

    REQUIRE_THROWS(adbcpp::list(connection, "/sdcard"));
}
