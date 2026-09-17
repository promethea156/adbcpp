#include "adbcpp/app.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "adbcpp/connection.hpp"
#include "adbcpp/protocol/commands.hpp"
#include "adbcpp/protocol/message.hpp"
#include "adbcpp/testing/mock_transport.hpp"

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

// A stream is keyed by two ids, and the ids are relative to the sender, so the
// device's id for a stream is our remote id and ours is our local id. The device
// numbers its first stream 7 and the next ones 8, 9, ...; ours are 2, 3, ....
constexpr std::uint32_t kDeviceId = 7;
constexpr std::uint32_t kLocalId = 2;
constexpr std::uint32_t kSecondDeviceId = 8;
constexpr std::uint32_t kSecondLocalId = 3;
constexpr std::uint32_t kThirdDeviceId = 9;
constexpr std::uint32_t kThirdLocalId = 4;
constexpr std::uint32_t kFourthDeviceId = 10;
constexpr std::uint32_t kFourthLocalId = 5;

// The device's `arg1` is the largest payload it accepts from us, so it has to be
// at least as large as a 64 KiB sync chunk. A real device advertises 1 MiB, like
// adb's own `MAX_PAYLOAD`.
constexpr std::uint32_t kDeviceMaxData = 1024u * 1024u;

// Feeds the device's CNXN with the given feature list, then the OKAY that accepts
// the first stream's OPEN. The device accepts every stream this way, so the frames
// for the later streams follow in the order the library opens them.
void feed_device(adbcpp::testing::MockTransport &transport, const std::string &features)
{
    const std::string banner = "device::features=" + features;
    feed_frame(transport, make_message(adbcpp::protocol::kCnxn, 0x01000001u, kDeviceMaxData, bytes_of(banner)),
               bytes_of(banner));
    feed_frame(transport, make_message(adbcpp::protocol::kOkay, kDeviceId, kLocalId));
}

// Feeds the OKAY that accepts the OPEN of a later stream.
void feed_open_on(adbcpp::testing::MockTransport &transport, std::uint32_t device_id, std::uint32_t local_id)
{
    feed_frame(transport, make_message(adbcpp::protocol::kOkay, device_id, local_id));
}

// Feeds `data` inside a WRTE from the device. A sync response and a shell_v2
// packet are not ADB messages, so they travel inside a WRTE payload.
void feed_wrte_on(adbcpp::testing::MockTransport &transport, std::uint32_t device_id, std::uint32_t local_id,
                  std::span<const std::byte> data)
{
    feed_frame(transport, make_message(adbcpp::protocol::kWrte, device_id, local_id, data), data);
}

// A STAT v2 response: id, error, dev, ino, mode, nlink, uid, gid, size, atime,
// mtime, ctime. ENOENT means the destination does not exist yet, so `push` sends
// the remote path as it is.
void feed_stat_missing_on(adbcpp::testing::MockTransport &transport, std::uint32_t device_id, std::uint32_t local_id)
{
    std::vector<std::byte> stat(72);
    write_u32_le(stat.data(), adbcpp::protocol::make_command('S', 'T', 'A', '2'));
    write_u32_le(stat.data() + 4, 2u); // ENOENT
    write_u64_le(stat.data() + 8, 0);  // dev
    write_u64_le(stat.data() + 16, 0); // ino
    write_u32_le(stat.data() + 24, 0); // mode
    write_u32_le(stat.data() + 28, 1); // nlink
    write_u32_le(stat.data() + 32, 0); // uid
    write_u32_le(stat.data() + 36, 0); // gid
    write_u64_le(stat.data() + 40, 0); // size
    write_u64_le(stat.data() + 48, 0); // atime
    write_u64_le(stat.data() + 56, 0); // mtime
    write_u64_le(stat.data() + 64, 0); // ctime
    feed_wrte_on(transport, device_id, local_id, stat);
}

// Feeds the OKAY the device acknowledges a completed `SEND` with.
void feed_status_okay_on(adbcpp::testing::MockTransport &transport, std::uint32_t device_id, std::uint32_t local_id)
{
    std::vector<std::byte> status(8);
    write_u32_le(status.data(), adbcpp::protocol::make_command('O', 'K', 'A', 'Y'));
    write_u32_le(status.data() + 4, 0u);
    feed_wrte_on(transport, device_id, local_id, status);
}

// Feeds one shell_v2 packet inside a WRTE: a one-byte id, a four-byte
// little-endian length, and the data.
void feed_shell_on(adbcpp::testing::MockTransport &transport, std::uint32_t device_id, std::uint32_t local_id,
                   std::uint8_t id, std::span<const std::byte> data)
{
    std::vector<std::byte> packet(5 + data.size());
    packet[0] = static_cast<std::byte>(id);
    packet[1] = static_cast<std::byte>(data.size() & 0xFFu);
    packet[2] = static_cast<std::byte>((data.size() >> 8) & 0xFFu);
    packet[3] = static_cast<std::byte>((data.size() >> 16) & 0xFFu);
    packet[4] = static_cast<std::byte>((data.size() >> 24) & 0xFFu);
    std::copy(data.begin(), data.end(), packet.begin() + 5);
    feed_wrte_on(transport, device_id, local_id, packet);
}

// Feeds a whole shell command on the stream the device opens next: the OKAY that
// accepts its OPEN, its stdout, its exit packet, and its CLOSE.
void feed_run_on(adbcpp::testing::MockTransport &transport, std::uint32_t device_id, std::uint32_t local_id,
                 const std::string &output, std::uint8_t exit_code)
{
    feed_open_on(transport, device_id, local_id);
    feed_shell_on(transport, device_id, local_id, 1, bytes_of(output));
    const std::array<std::byte, 1> status{static_cast<std::byte>(exit_code)};
    feed_shell_on(transport, device_id, local_id, 3, status);
    feed_frame(transport, make_message(adbcpp::protocol::kClse, device_id, local_id));
}

// The frames the device sends while `install` runs: its CNXN, then the streams
// `push` and the two shell commands open. The install succeeds when the package
// manager prints `Success`.
void feed_install(adbcpp::testing::MockTransport &transport, const std::string &pm_output, std::uint8_t pm_exit_code)
{
    feed_device(transport, "shell_v2,stat_v2");
    // `push` opens one stream for its STAT and one for its SEND.
    feed_stat_missing_on(transport, kDeviceId, kLocalId);
    feed_open_on(transport, kSecondDeviceId, kSecondLocalId);
    feed_status_okay_on(transport, kSecondDeviceId, kSecondLocalId);
    // `pm install` and the `rm -f` that removes the pushed APK.
    feed_run_on(transport, kThirdDeviceId, kThirdLocalId, pm_output, pm_exit_code);
    feed_run_on(transport, kFourthDeviceId, kFourthLocalId, "", 0);
}

// A scratch APK, removed before it is used.
std::filesystem::path temp_file(const std::string &name, const std::string &contents)
{
    const auto path = std::filesystem::temp_directory_path() / ("adbcpp_" + name);
    std::filesystem::remove(path);
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << contents;
    }
    return path;
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

bool contains(const std::vector<std::byte> &written, const std::string &text)
{
    const auto bytes = bytes_of(text);
    return std::search(written.begin(), written.end(), bytes.begin(), bytes.end()) != written.end();
}

std::size_t count_ids(const std::vector<std::byte> &written, const char *four)
{
    std::array<std::byte, 4> id{};
    write_u32_le(id.data(), adbcpp::protocol::make_command(four[0], four[1], four[2], four[3]));
    std::size_t count = 0;
    auto found = std::search(written.begin(), written.end(), id.begin(), id.end());
    while (found != written.end())
    {
        ++count;
        found = std::search(found + 1, written.end(), id.begin(), id.end());
    }
    return count;
}

} // namespace

TEST_CASE("install reports the package manager's success", "[app]")
{
    adbcpp::testing::MockTransport transport;
    feed_install(transport, "Success\n", 0);

    const auto apk = temp_file("install_success.apk", "not really an apk\n");
    adbcpp::Connection connection(transport);
    const auto result = adbcpp::install(connection, apk);
    std::filesystem::remove(apk);

    REQUIRE(result.success);
    REQUIRE(result.output == "Success\n");
    REQUIRE(result.exit_code == 0);
    REQUIRE(result.failure_reason().empty());
}

TEST_CASE("install pushes the APK to /data/local/tmp and installs it from there", "[app]")
{
    adbcpp::testing::MockTransport transport;
    feed_install(transport, "Success\n", 0);

    const auto apk = temp_file("install_push.apk", "data");
    adbcpp::Connection connection(transport);
    adbcpp::install(connection, apk);
    std::filesystem::remove(apk);

    const auto &written = transport.written();

    // SEND(id, path_length, "<path>,<mode>"): the pushed APK.
    const auto send_offset = find_id(written, "SEND");
    const std::uint32_t spec_length = read_u32_le(written.data() + send_offset + 4);
    const std::string spec(reinterpret_cast<const char *>(written.data() + send_offset + 8), spec_length);
    REQUIRE(spec.starts_with("/data/local/tmp/adbcpp_install_push.apk,"));

    // `pm install` runs over the shell service, which the device runs through
    // `sh -c`, so the path is single-quoted.
    REQUIRE(contains(written, "pm install '/data/local/tmp/adbcpp_install_push.apk'"));
}

TEST_CASE("install removes the pushed APK", "[app]")
{
    adbcpp::testing::MockTransport transport;
    feed_install(transport, "Success\n", 0);

    const auto apk = temp_file("install_cleanup.apk", "data");
    adbcpp::Connection connection(transport);
    adbcpp::install(connection, apk);
    std::filesystem::remove(apk);

    REQUIRE(contains(transport.written(), "rm -f '/data/local/tmp/adbcpp_install_cleanup.apk'"));
}

TEST_CASE("install removes the pushed APK when the package manager rejects it", "[app]")
{
    adbcpp::testing::MockTransport transport;
    feed_install(transport, "Failure [INSTALL_FAILED_INVALID_APK]\n", 1);

    const auto apk = temp_file("install_cleanup_failed.apk", "data");
    adbcpp::Connection connection(transport);
    adbcpp::install(connection, apk);
    std::filesystem::remove(apk);

    REQUIRE(contains(transport.written(), "rm -f '/data/local/tmp/adbcpp_install_cleanup_failed.apk'"));
}

TEST_CASE("install passes the options through to the package manager", "[app]")
{
    adbcpp::testing::MockTransport transport;
    feed_install(transport, "Success\n", 0);

    const auto apk = temp_file("install_options.apk", "data");
    adbcpp::Connection connection(transport);
    adbcpp::install(connection, apk, "-r -d");
    std::filesystem::remove(apk);

    REQUIRE(contains(transport.written(), "pm install -r -d '/data/local/tmp/adbcpp_install_options.apk'"));
}

TEST_CASE("install reports a rejected APK as a failure", "[app]")
{
    adbcpp::testing::MockTransport transport;
    feed_install(transport, "Failure [INSTALL_FAILED_INVALID_APK: Failed to extract native libraries]\n", 1);

    const auto apk = temp_file("install_failed.apk", "not really an apk\n");
    adbcpp::Connection connection(transport);
    const auto result = adbcpp::install(connection, apk);
    std::filesystem::remove(apk);

    REQUIRE_FALSE(result.success);
    REQUIRE(result.exit_code == 1);
    REQUIRE(result.failure_reason() == "INSTALL_FAILED_INVALID_APK: Failed to extract native libraries");
}

TEST_CASE("install splits a file larger than 64 KiB into several chunks", "[app]")
{
    adbcpp::testing::MockTransport transport;
    feed_install(transport, "Success\n", 0);

    // 64 KiB is SYNC_DATA_MAX, so this needs two chunks.
    const auto apk = temp_file("install_large.apk", std::string(70u * 1024u, 'a'));
    adbcpp::Connection connection(transport);
    adbcpp::install(connection, apk);
    std::filesystem::remove(apk);

    REQUIRE(count_ids(transport.written(), "DATA") == 2u);
}

TEST_CASE("install quotes an APK name that contains a space", "[app]")
{
    adbcpp::testing::MockTransport transport;
    feed_install(transport, "Success\n", 0);

    const auto apk = temp_file("install my app.apk", "data");
    adbcpp::Connection connection(transport);
    adbcpp::install(connection, apk);
    std::filesystem::remove(apk);

    REQUIRE(contains(transport.written(), "pm install '/data/local/tmp/adbcpp_install my app.apk'"));
}

TEST_CASE("install rejects a local path that is not a regular file", "[app]")
{
    adbcpp::testing::MockTransport transport;
    feed_device(transport, "shell_v2,stat_v2");

    adbcpp::Connection connection(transport);

    REQUIRE_THROWS(adbcpp::install(connection, std::filesystem::temp_directory_path()));
}

TEST_CASE("uninstall reports the package manager's success", "[app]")
{
    adbcpp::testing::MockTransport transport;
    feed_device(transport, "shell_v2,stat_v2");
    feed_run_on(transport, kDeviceId, kLocalId, "Success\n", 0);

    adbcpp::Connection connection(transport);
    const auto result = adbcpp::uninstall(connection, "com.example.app");

    REQUIRE(result.success);
    REQUIRE(result.exit_code == 0);
    REQUIRE(result.failure_reason().empty());
    REQUIRE(contains(transport.written(), "pm uninstall 'com.example.app'"));
}

TEST_CASE("uninstall keeps the data when asked", "[app]")
{
    adbcpp::testing::MockTransport transport;
    feed_device(transport, "shell_v2,stat_v2");
    feed_run_on(transport, kDeviceId, kLocalId, "Success\n", 0);

    adbcpp::Connection connection(transport);
    const auto result = adbcpp::uninstall(connection, "com.example.app", true);

    REQUIRE(result.success);
    REQUIRE(contains(transport.written(), "pm uninstall -k 'com.example.app'"));
}

TEST_CASE("uninstall reports a package the device cannot remove", "[app]")
{
    adbcpp::testing::MockTransport transport;
    feed_device(transport, "shell_v2,stat_v2");
    feed_run_on(transport, kDeviceId, kLocalId, "Failure [DELETE_FAILED_INTERNAL_ERROR]\n", 1);

    adbcpp::Connection connection(transport);
    const auto result = adbcpp::uninstall(connection, "com.example.missing");

    REQUIRE_FALSE(result.success);
    REQUIRE(result.exit_code == 1);
    REQUIRE(result.failure_reason() == "DELETE_FAILED_INTERNAL_ERROR");
}
