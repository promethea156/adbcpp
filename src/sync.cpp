#include "adbcpp/sync.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "adbcpp/protocol/commands.hpp"
#include "adbcpp/stream.hpp"

namespace adbcpp
{
namespace
{

// The sync service's request and response ids, from AOSP's
// file_sync_protocol.h. Like the ADB commands they are the little-endian
// encoding of four ASCII characters, so they reuse `make_command`:
//
//   https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/file_sync_protocol.h
//
// `kListV1` and `kListV2` are the two LIST requests, `kDentV1` and `kDentV2`
// the matching directory entries, `kDone` ends a listing, `kFail` rejects the
// request, and `kQuit` leaves sync mode.
constexpr std::uint32_t kListV1 = protocol::make_command('L', 'I', 'S', 'T');
constexpr std::uint32_t kListV2 = protocol::make_command('L', 'I', 'S', '2');
constexpr std::uint32_t kDentV1 = protocol::make_command('D', 'E', 'N', 'T');
constexpr std::uint32_t kDentV2 = protocol::make_command('D', 'N', 'T', '2');
constexpr std::uint32_t kDone = protocol::make_command('D', 'O', 'N', 'E');
constexpr std::uint32_t kFail = protocol::make_command('F', 'A', 'I', 'L');
constexpr std::uint32_t kQuit = protocol::make_command('Q', 'U', 'I', 'T');

// A sync request is an id followed by a path length; the path itself is not
// null-terminated. The daemon rejects a path longer than 1024 bytes.
constexpr std::size_t kRequestSize = 8;
constexpr std::size_t kMaxPathLength = 1024;

// A DENT body follows its id. v1 is `mode`, `size`, `mtime`, `namelen`; v2
// inserts an error and the full POSIX metadata before `namelen`.
constexpr std::size_t kDentV1BodySize = 16;
constexpr std::size_t kDentV2BodySize = 72;
constexpr std::size_t kDentV1NameLengthOffset = 12;
constexpr std::size_t kDentV2NameLengthOffset = 68;
// The name length is capped at NAME_MAX, which is 255 on Linux, like adb.
constexpr std::size_t kMaxNameLength = 255;

// The `ls_v2` feature selects the v2 DENT form, exactly like adb.
constexpr std::string_view kLsV2Feature = "ls_v2";

// Every binary integer in sync mode is little-endian, like the ADB header.
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

std::uint64_t read_u64_le(const std::byte *in) noexcept
{
    return static_cast<std::uint64_t>(read_u32_le(in)) | (static_cast<std::uint64_t>(read_u32_le(in + 4)) << 32);
}

} // namespace

std::vector<DirEntry> list(Connection &connection, std::string_view path)
{
    if (path.size() > kMaxPathLength)
    {
        throw std::runtime_error("adbcpp: the path is longer than the sync limit of 1024 bytes");
    }

    // `ls_v2` selects the v2 DENT form. Matching adb, it is an exact feature
    // match rather than a substring search.
    const bool v2 = connection.supports_feature(kLsV2Feature);

    // Requesting the `sync:` service puts the stream in sync mode.
    Stream stream(connection, "sync:");

    // LIST(id, path_length, "path"). A sync request is not an ADB message, so it
    // has no 24-byte header, and the path is not null-terminated.
    std::vector<std::byte> request(kRequestSize + path.size());
    write_u32_le(request.data(), v2 ? kListV2 : kListV1);
    write_u32_le(request.data() + 4, static_cast<std::uint32_t>(path.size()));
    std::copy_n(reinterpret_cast<const std::byte *>(path.data()), static_cast<std::ptrdiff_t>(path.size()),
                request.begin() + static_cast<std::ptrdiff_t>(kRequestSize));
    stream.write(request);

    const std::size_t body_size = v2 ? kDentV2BodySize : kDentV1BodySize;
    const std::size_t name_length_offset = v2 ? kDentV2NameLengthOffset : kDentV1NameLengthOffset;

    std::vector<DirEntry> entries;
    while (true)
    {
        // Every response starts with a four-byte id.
        std::array<std::byte, 4> id_bytes{};
        stream.read(id_bytes);
        const std::uint32_t id = read_u32_le(id_bytes.data());

        // FAIL is `sync_data { id, size }` followed by the reason, which the
        // daemon sends when the request itself is rejected, for example when the
        // path is too long. Its body is a length, not a DENT body.
        if (id == kFail)
        {
            std::array<std::byte, 4> length_bytes{};
            stream.read(length_bytes);
            const std::uint32_t length = read_u32_le(length_bytes.data());
            std::vector<std::byte> reason(length);
            stream.read(reason);
            throw std::runtime_error("adbcpp: the sync request failed: " +
                                     std::string(reinterpret_cast<const char *>(reason.data()), reason.size()));
        }

        // DONE is a full DENT struct whose id is DONE, so its body is read too
        // to keep the stream in sync, and then the listing is over.
        std::vector<std::byte> body(body_size);
        stream.read(body);
        if (id == kDone)
        {
            break;
        }

        if (id != (v2 ? kDentV2 : kDentV1))
        {
            throw std::runtime_error("adbcpp: unexpected sync response");
        }

        // The name always follows, even when the entry failed, so it is read
        // first to keep the stream in sync.
        const std::uint32_t name_length = read_u32_le(body.data() + name_length_offset);
        if (name_length > kMaxNameLength)
        {
            throw std::runtime_error("adbcpp: the entry name is longer than 255 bytes");
        }
        std::vector<std::byte> name(name_length);
        stream.read(name);

        // v2 reports a failed `lstat` per entry instead of dropping it. The
        // error is the first field of the body.
        if (v2 && read_u32_le(body.data()) != 0)
        {
            continue;
        }

        DirEntry entry;
        entry.name.assign(reinterpret_cast<const char *>(name.data()), name.size());
        if (v2)
        {
            entry.mode = read_u32_le(body.data() + 20);
            entry.size = read_u64_le(body.data() + 36);
            entry.mtime = static_cast<std::int64_t>(read_u64_le(body.data() + 52));
        }
        else
        {
            entry.mode = read_u32_le(body.data() + 0);
            entry.size = read_u32_le(body.data() + 4);
            entry.mtime = read_u32_le(body.data() + 8);
        }
        entries.push_back(std::move(entry));
    }

    // QUIT leaves sync mode, after which the daemon closes the stream.
    std::array<std::byte, kRequestSize> quit{};
    write_u32_le(quit.data(), kQuit);
    stream.write(quit);

    return entries;
}

} // namespace adbcpp
