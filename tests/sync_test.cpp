#include "adbcpp/sync.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "adbcpp/connection.hpp"
#include "adbcpp/protocol/commands.hpp"
#include "adbcpp/protocol/message.hpp"
#include "adbcpp/testing/mock_transport.hpp"

using adbcpp::protocol::Message;

namespace
{

// Returns the value of a `Result` the test expects to succeed, failing the test
// otherwise.
template <typename T>
T unwrap(adbcpp::Result<T> result)
{
    REQUIRE(result.has_value());
    return std::move(*result);
}

// Fails the test unless a `Status` reports success.
void unwrap(adbcpp::Status status)
{
    REQUIRE(status.has_value());
}

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

std::uint32_t read_u32_le(const std::byte *in)
{
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[0])) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[1])) << 8) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[2])) << 16) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[3])) << 24);
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
    message.data_length = *adbcpp::protocol::Message::data_length_of(payload);
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

// A stream is keyed by two ids, and the ids are relative to the sender, so the
// device's id for a stream is our remote id and ours is our local id. The device
// numbers its first stream 7 and the next ones 8, 9, ...; ours are 2, 3, ....
constexpr std::uint32_t kDeviceId = 7;
constexpr std::uint32_t kLocalId = 2;
constexpr std::uint32_t kSecondDeviceId = 8;
constexpr std::uint32_t kSecondLocalId = 3;

// Feeds the device's CNXN with the given feature list, then the OKAY that
// accepts the first stream's OPEN.
void feed_device(adbcpp::testing::MockTransport &transport, const std::string &features)
{
    const std::string banner = "device::features=" + features;
    feed_frame(transport, make_message(adbcpp::protocol::kCnxn, 0x01000001u, 4096u, bytes_of(banner)),
               bytes_of(banner));
    feed_frame(transport, make_message(adbcpp::protocol::kOkay, kDeviceId, kLocalId));
}

// Feeds the OKAY that accepts a second stream's OPEN. Each of `list`, `pull`,
// `stat`, and `push` opens its own `sync:` stream, and `push` opens two: one for
// its STAT and one for its SEND.
void feed_second_open(adbcpp::testing::MockTransport &transport)
{
    feed_frame(transport, make_message(adbcpp::protocol::kOkay, kSecondDeviceId, kSecondLocalId));
}

// Feeds `data` inside a WRTE from the device on the given stream. A sync
// response is not an ADB message, so it travels inside a WRTE payload, exactly
// like shell output.
void feed_sync_on(adbcpp::testing::MockTransport &transport, std::uint32_t arg0, std::uint32_t arg1,
                  std::span<const std::byte> data)
{
    feed_frame(transport, make_message(adbcpp::protocol::kWrte, arg0, arg1, data), data);
}

// Feeds `data` inside a WRTE from the device on the first stream.
void feed_sync(adbcpp::testing::MockTransport &transport, std::span<const std::byte> data)
{
    feed_sync_on(transport, kDeviceId, kLocalId, data);
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

// DONE for a listing is a full DENT struct with its id set to DONE.
void feed_list_done(adbcpp::testing::MockTransport &transport, bool v2)
{
    const std::size_t size = v2 ? 76 : 20;
    std::vector<std::byte> done(size);
    write_u32_le(done.data(), adbcpp::protocol::make_command('D', 'O', 'N', 'E'));
    feed_sync(transport, done);
}

// Feeds one RECV chunk: id DATA, the chunk size, and the bytes.
void feed_data(adbcpp::testing::MockTransport &transport, std::span<const std::byte> data)
{
    std::vector<std::byte> chunk(8 + data.size());
    write_u32_le(chunk.data(), adbcpp::protocol::make_command('D', 'A', 'T', 'A'));
    write_u32_le(chunk.data() + 4, static_cast<std::uint32_t>(data.size()));
    std::copy_n(data.begin(), static_cast<std::ptrdiff_t>(data.size()), chunk.begin() + static_cast<std::ptrdiff_t>(8));
    feed_sync(transport, chunk);
}

// DONE for a transfer is `sync_data { id, size }`, not a DENT struct; the size is
// ignored.
void feed_recv_done(adbcpp::testing::MockTransport &transport)
{
    std::vector<std::byte> done(8);
    write_u32_le(done.data(), adbcpp::protocol::make_command('D', 'O', 'N', 'E'));
    write_u32_le(done.data() + 4, 0u);
    feed_sync(transport, done);
}

// A STAT v2 response: id, error, dev, ino, mode, nlink, uid, gid, size, atime,
// mtime, ctime. The field offsets are the ones `stat` parses.
void feed_stat_v2(adbcpp::testing::MockTransport &transport, std::uint32_t error, std::uint32_t mode,
                  std::uint64_t size, std::int64_t mtime)
{
    std::vector<std::byte> stat(72);
    write_u32_le(stat.data(), adbcpp::protocol::make_command('S', 'T', 'A', '2'));
    write_u32_le(stat.data() + 4, error);
    write_u64_le(stat.data() + 8, 0);  // dev
    write_u64_le(stat.data() + 16, 0); // ino
    write_u32_le(stat.data() + 24, mode);
    write_u32_le(stat.data() + 28, 1); // nlink
    write_u32_le(stat.data() + 32, 0); // uid
    write_u32_le(stat.data() + 36, 0); // gid
    write_u64_le(stat.data() + 40, size);
    write_u64_le(stat.data() + 48, 0); // atime
    write_u64_le(stat.data() + 56, static_cast<std::uint64_t>(mtime));
    write_u64_le(stat.data() + 64, 0); // ctime
    feed_sync(transport, stat);
}

// A STAT v1 response: id, mode, size, mtime.
void feed_stat_v1(adbcpp::testing::MockTransport &transport, std::uint32_t mode, std::uint32_t size,
                  std::uint32_t mtime)
{
    std::vector<std::byte> stat(16);
    write_u32_le(stat.data(), adbcpp::protocol::make_command('S', 'T', 'A', 'T'));
    write_u32_le(stat.data() + 4, mode);
    write_u32_le(stat.data() + 8, size);
    write_u32_le(stat.data() + 12, mtime);
    feed_sync(transport, stat);
}

// A reply the device acknowledges a request with on the given stream:
// `sync_status { id, msglen }`.
void feed_status_on(adbcpp::testing::MockTransport &transport, std::uint32_t arg0, std::uint32_t arg1, std::uint32_t id)
{
    std::vector<std::byte> status(8);
    write_u32_le(status.data(), id);
    write_u32_le(status.data() + 4, 0u);
    feed_sync_on(transport, arg0, arg1, status);
}

// A scratch path for a pulled file, removed before it is used.
std::filesystem::path temp_file(const std::string &name)
{
    const auto path = std::filesystem::temp_directory_path() / ("adbcpp_" + name);
    std::filesystem::remove(path);
    return path;
}

// A scratch file for a push, closed before it is used.
std::filesystem::path temp_file(const std::string &name, const std::string &contents)
{
    const auto path = temp_file(name);
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << contents;
    }
    return path;
}

std::string read_file(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

// The offset of a four-byte sync id in the bytes the host wrote.
std::size_t find_id(const std::vector<std::byte> &written, const char *four)
{
    std::array<std::byte, 4> id{};
    write_u32_le(id.data(), adbcpp::protocol::make_command(four[0], four[1], four[2], four[3]));
    const auto found = std::search(written.begin(), written.end(), id.begin(), id.end());
    REQUIRE(found != written.end());
    return static_cast<std::size_t>(std::distance(written.begin(), found));
}

void feed_fail_on(adbcpp::testing::MockTransport &transport, std::uint32_t arg0, std::uint32_t arg1,
                  const std::string &reason)
{
    std::vector<std::byte> fail(8 + reason.size());
    write_u32_le(fail.data(), adbcpp::protocol::make_command('F', 'A', 'I', 'L'));
    write_u32_le(fail.data() + 4, static_cast<std::uint32_t>(reason.size()));
    std::copy_n(reinterpret_cast<const std::byte *>(reason.data()), static_cast<std::ptrdiff_t>(reason.size()),
                fail.begin() + static_cast<std::ptrdiff_t>(8));
    feed_sync_on(transport, arg0, arg1, fail);
}

void feed_fail(adbcpp::testing::MockTransport &transport, const std::string &reason)
{
    feed_fail_on(transport, kDeviceId, kLocalId, reason);
}

} // namespace

TEST_CASE("list parses the v1 DENT entries", "[sync]")
{
    adbcpp::testing::MockTransport transport;
    feed_device(transport, "shell_v2,cmd");
    feed_dent_v1(transport, 0040755u, 0u, 123u, ".");
    feed_dent_v1(transport, 0100644u, 42u, 456u, "file.txt");
    feed_list_done(transport, false);

    auto connection = unwrap(adbcpp::Connection::connect(transport));
    const auto entries = unwrap(adbcpp::list(connection, "/sdcard"));

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
    feed_list_done(transport, true);

    auto connection = unwrap(adbcpp::Connection::connect(transport));
    const auto entries = unwrap(adbcpp::list(connection, "/"));

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
    feed_list_done(transport, true);

    auto connection = unwrap(adbcpp::Connection::connect(transport));
    unwrap(adbcpp::list(connection, "/sdcard"));

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
    feed_list_done(transport, false);

    auto connection = unwrap(adbcpp::Connection::connect(transport));
    unwrap(adbcpp::list(connection, "/"));

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
    feed_list_done(transport, true);

    auto connection = unwrap(adbcpp::Connection::connect(transport));
    const auto entries = unwrap(adbcpp::list(connection, "/"));

    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].name == "file.txt");
    REQUIRE(entries[0].size == 10u);
}

TEST_CASE("list rejects a path longer than the sync limit", "[sync]")
{
    adbcpp::testing::MockTransport transport;
    feed_device(transport, "shell_v2,cmd");

    auto connection = unwrap(adbcpp::Connection::connect(transport));
    const std::string path(1025, 'a');

    const auto entries = adbcpp::list(connection, path);
    REQUIRE_FALSE(entries.has_value());
    REQUIRE(entries.error().code == adbcpp::ErrorCode::InvalidArgument);
}

TEST_CASE("list returns an error when the sync request fails", "[sync]")
{
    adbcpp::testing::MockTransport transport;
    feed_device(transport, "shell_v2,cmd");
    feed_fail(transport, "path too long");

    auto connection = unwrap(adbcpp::Connection::connect(transport));

    const auto entries = adbcpp::list(connection, "/sdcard");
    REQUIRE_FALSE(entries.has_value());
    REQUIRE(entries.error().code == adbcpp::ErrorCode::Device);
}

TEST_CASE("pull writes the received chunks to the local file", "[sync]")
{
    adbcpp::testing::MockTransport transport;
    feed_device(transport, "shell_v2,cmd");
    feed_data(transport, bytes_of("hello "));
    feed_data(transport, bytes_of("world\n"));
    feed_recv_done(transport);

    const auto local = temp_file("pull_chunks.bin");
    auto connection = unwrap(adbcpp::Connection::connect(transport));
    unwrap(adbcpp::pull(connection, "/sdcard/file.txt", local));

    REQUIRE(read_file(local) == "hello world\n");
    std::filesystem::remove(local);
}

TEST_CASE("pull sends the RECV request with the path", "[sync]")
{
    adbcpp::testing::MockTransport transport;
    feed_device(transport, "shell_v2,cmd");
    feed_recv_done(transport);

    const auto local = temp_file("pull_request.bin");
    auto connection = unwrap(adbcpp::Connection::connect(transport));
    unwrap(adbcpp::pull(connection, "/sdcard/file.txt", local));

    // RECV(id, path_length, "path"), with no null terminator.
    const std::string path = "/sdcard/file.txt";
    std::vector<std::byte> request(8 + path.size());
    write_u32_le(request.data(), adbcpp::protocol::make_command('R', 'E', 'C', 'V'));
    write_u32_le(request.data() + 4, static_cast<std::uint32_t>(path.size()));
    std::copy_n(reinterpret_cast<const std::byte *>(path.data()), static_cast<std::ptrdiff_t>(path.size()),
                request.begin() + static_cast<std::ptrdiff_t>(8));

    const auto &written = transport.written();
    REQUIRE(std::search(written.begin(), written.end(), request.begin(), request.end()) != written.end());
    std::filesystem::remove(local);
}

TEST_CASE("pull leaves sync mode with QUIT", "[sync]")
{
    adbcpp::testing::MockTransport transport;
    feed_device(transport, "shell_v2,cmd");
    feed_recv_done(transport);

    const auto local = temp_file("pull_quit.bin");
    auto connection = unwrap(adbcpp::Connection::connect(transport));
    unwrap(adbcpp::pull(connection, "/sdcard/file.txt", local));

    std::vector<std::byte> quit(8);
    write_u32_le(quit.data(), adbcpp::protocol::make_command('Q', 'U', 'I', 'T'));
    write_u32_le(quit.data() + 4, 0u);

    const auto &written = transport.written();
    REQUIRE(std::search(written.begin(), written.end(), quit.begin(), quit.end()) != written.end());
    std::filesystem::remove(local);
}

TEST_CASE("pull writes an empty file when no chunks are sent", "[sync]")
{
    adbcpp::testing::MockTransport transport;
    feed_device(transport, "shell_v2,cmd");
    feed_recv_done(transport);

    const auto local = temp_file("pull_empty.bin");
    auto connection = unwrap(adbcpp::Connection::connect(transport));
    unwrap(adbcpp::pull(connection, "/sdcard/empty", local));

    REQUIRE(std::filesystem::exists(local));
    REQUIRE(read_file(local).empty());
    std::filesystem::remove(local);
}

TEST_CASE("pull returns an error when the request fails", "[sync]")
{
    adbcpp::testing::MockTransport transport;
    feed_device(transport, "shell_v2,cmd");
    feed_fail(transport, "open failed");

    const auto local = temp_file("pull_fail.bin");
    auto connection = unwrap(adbcpp::Connection::connect(transport));

    const auto status = adbcpp::pull(connection, "/sdcard/file.txt", local);
    REQUIRE_FALSE(status.has_value());
    REQUIRE(status.error().code == adbcpp::ErrorCode::Device);
    std::filesystem::remove(local);
}

TEST_CASE("pull returns an error when a chunk is larger than 64 KiB", "[sync]")
{
    adbcpp::testing::MockTransport transport;
    feed_device(transport, "shell_v2,cmd");

    // A DATA header whose size exceeds SYNC_DATA_MAX, with no data behind it.
    std::vector<std::byte> chunk(8);
    write_u32_le(chunk.data(), adbcpp::protocol::make_command('D', 'A', 'T', 'A'));
    write_u32_le(chunk.data() + 4, 64u * 1024u + 1u);
    feed_sync(transport, chunk);

    const auto local = temp_file("pull_big_chunk.bin");
    auto connection = unwrap(adbcpp::Connection::connect(transport));

    const auto status = adbcpp::pull(connection, "/sdcard/file.txt", local);
    REQUIRE_FALSE(status.has_value());
    REQUIRE(status.error().code == adbcpp::ErrorCode::Protocol);
    std::filesystem::remove(local);
}

TEST_CASE("pull rejects a path longer than the sync limit", "[sync]")
{
    adbcpp::testing::MockTransport transport;
    feed_device(transport, "shell_v2,cmd");

    auto connection = unwrap(adbcpp::Connection::connect(transport));
    const std::string path(1025, 'a');

    const auto status = adbcpp::pull(connection, path, temp_file("pull_long_path.bin"));
    REQUIRE_FALSE(status.has_value());
    REQUIRE(status.error().code == adbcpp::ErrorCode::InvalidArgument);
}

TEST_CASE("stat returns the v2 metadata", "[sync]")
{
    adbcpp::testing::MockTransport transport;
    feed_device(transport, "shell_v2,stat_v2");
    feed_stat_v2(transport, 0u, 0100644u, 42u, 456);

    auto connection = unwrap(adbcpp::Connection::connect(transport));
    const auto info = unwrap(adbcpp::stat(connection, "/sdcard/file.txt"));

    REQUIRE(info.has_value());
    REQUIRE(info->is_regular());
    REQUIRE(info->size == 42u);
    REQUIRE(info->mtime == 456);
}

TEST_CASE("stat reports a missing path as nothing", "[sync]")
{
    adbcpp::testing::MockTransport transport;
    feed_device(transport, "shell_v2,stat_v2");
    // ENOENT, in the v2 form's leading error field.
    feed_stat_v2(transport, 2u, 0u, 0u, 0);

    auto connection = unwrap(adbcpp::Connection::connect(transport));
    const auto info = unwrap(adbcpp::stat(connection, "/sdcard/missing"));

    REQUIRE_FALSE(info.has_value());
}

TEST_CASE("stat uses the v1 form when stat_v2 is not advertised", "[sync]")
{
    adbcpp::testing::MockTransport transport;
    feed_device(transport, "shell_v2");
    feed_stat_v1(transport, 0040755u, 4096u, 456);

    auto connection = unwrap(adbcpp::Connection::connect(transport));
    const auto info = unwrap(adbcpp::stat(connection, "/sdcard"));

    REQUIRE(info.has_value());
    REQUIRE(info->is_directory());
    REQUIRE(info->size == 4096u);
    REQUIRE(info->mtime == 456);
}

TEST_CASE("stat reports an all-zero v1 response as nothing", "[sync]")
{
    adbcpp::testing::MockTransport transport;
    feed_device(transport, "shell_v2");
    // The v1 form has no error field, so a missing path is all zeros.
    feed_stat_v1(transport, 0u, 0u, 0u);

    auto connection = unwrap(adbcpp::Connection::connect(transport));
    const auto info = unwrap(adbcpp::stat(connection, "/sdcard/missing"));

    REQUIRE_FALSE(info.has_value());
}

TEST_CASE("push sends the file as DATA chunks and reads the OKAY", "[sync]")
{
    adbcpp::testing::MockTransport transport;
    feed_device(transport, "shell_v2,stat_v2");
    // The destination does not exist yet, so no name is appended.
    feed_stat_v2(transport, 2u, 0u, 0u, 0);
    // `push` opens a second stream for its SEND, which the device accepts, and
    // then acknowledges the transfer on that stream.
    feed_second_open(transport);
    feed_status_on(transport, kSecondDeviceId, kSecondLocalId, adbcpp::protocol::make_command('O', 'K', 'A', 'Y'));

    const auto local = temp_file("push.txt", "hello world\n");
    auto connection = unwrap(adbcpp::Connection::connect(transport));
    unwrap(adbcpp::push(connection, local, "/sdcard/file.txt"));
    std::filesystem::remove(local);

    const auto &written = transport.written();

    // SEND(id, path_length, "path,mode"). The mode is the local file's, so only
    // the path is compared here.
    const auto send_offset = find_id(written, "SEND");
    const std::uint32_t spec_length = read_u32_le(written.data() + send_offset + 4);
    const std::string spec(reinterpret_cast<const char *>(written.data() + send_offset + 8), spec_length);
    REQUIRE(spec.starts_with("/sdcard/file.txt,"));

    // DATA(id, size, "hello world\n").
    const auto data_offset = find_id(written, "DATA");
    REQUIRE(read_u32_le(written.data() + data_offset + 4) == 12u);
    const std::string text = "hello world\n";
    const auto content = bytes_of(text);
    REQUIRE(std::search(written.begin(), written.end(), content.begin(), content.end()) != written.end());

    // DONE(id, mtime). The device applies the size as the file's modification
    // time, so it is close to now rather than zero.
    const auto done_offset = find_id(written, "DONE");
    const auto mtime = static_cast<std::int64_t>(read_u32_le(written.data() + done_offset + 4));
    REQUIRE(mtime > std::time(nullptr) - 60);
}

TEST_CASE("push appends the local name when the destination is a directory", "[sync]")
{
    adbcpp::testing::MockTransport transport;
    feed_device(transport, "shell_v2,stat_v2");
    // The destination exists and is a directory.
    feed_stat_v2(transport, 0u, 0040755u, 4096u, 0);
    feed_second_open(transport);
    feed_status_on(transport, kSecondDeviceId, kSecondLocalId, adbcpp::protocol::make_command('O', 'K', 'A', 'Y'));

    const auto local = temp_file("push_dir.txt", "data");
    auto connection = unwrap(adbcpp::Connection::connect(transport));
    unwrap(adbcpp::push(connection, local, "/sdcard"));
    std::filesystem::remove(local);

    const auto &written = transport.written();
    const auto send_offset = find_id(written, "SEND");
    const std::uint32_t spec_length = read_u32_le(written.data() + send_offset + 4);
    const std::string spec(reinterpret_cast<const char *>(written.data() + send_offset + 8), spec_length);
    REQUIRE(spec.starts_with("/sdcard/adbcpp_push_dir.txt,"));
}

TEST_CASE("push returns an error when the request is rejected", "[sync]")
{
    adbcpp::testing::MockTransport transport;
    feed_device(transport, "shell_v2,stat_v2");
    feed_stat_v2(transport, 2u, 0u, 0u, 0);
    feed_second_open(transport);
    feed_fail_on(transport, kSecondDeviceId, kSecondLocalId, "couldn't create file");

    const auto local = temp_file("push_fail.txt", "data");
    auto connection = unwrap(adbcpp::Connection::connect(transport));

    const auto status = adbcpp::push(connection, local, "/sdcard/file.txt");
    REQUIRE_FALSE(status.has_value());
    REQUIRE(status.error().code == adbcpp::ErrorCode::Device);
    std::filesystem::remove(local);
}

TEST_CASE("push rejects a local path that is not a regular file", "[sync]")
{
    adbcpp::testing::MockTransport transport;
    feed_device(transport, "shell_v2,stat_v2");

    auto connection = unwrap(adbcpp::Connection::connect(transport));

    const auto status = adbcpp::push(connection, std::filesystem::temp_directory_path(), "/sdcard/file.txt");
    REQUIRE_FALSE(status.has_value());
    REQUIRE(status.error().code == adbcpp::ErrorCode::InvalidArgument);
}

TEST_CASE("push rejects a remote path longer than the sync limit", "[sync]")
{
    adbcpp::testing::MockTransport transport;
    feed_device(transport, "shell_v2,stat_v2");

    const auto local = temp_file("push_long_path.txt", "data");
    auto connection = unwrap(adbcpp::Connection::connect(transport));

    const auto status = adbcpp::push(connection, local, std::string(1025, 'a'));
    REQUIRE_FALSE(status.has_value());
    REQUIRE(status.error().code == adbcpp::ErrorCode::InvalidArgument);
    std::filesystem::remove(local);
}
