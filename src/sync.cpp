#include "adbcpp/sync.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#    include <sys/stat.h>
#endif

#include "adbcpp/protocol/commands.hpp"
#include "adbcpp/stream.hpp"
#include "protocol/byte_order.hpp"

#if defined(ADBCPP_HAS_COMPRESSION)
#    include <span>

#    include "compression.hpp"
#endif

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
// the matching directory entries, `kRecvV1` retrieves a file, `kSendV1` stores
// one, `kStatV2` and `kStatV1` report a path's metadata, `kData` carries one
// chunk of a file, `kDone` ends a listing or a transfer, `kOkay` acknowledges a
// completed transfer, `kFail` rejects the request, and `kQuit` leaves sync mode.
constexpr std::uint32_t kListV1 = protocol::make_command('L', 'I', 'S', 'T');
constexpr std::uint32_t kListV2 = protocol::make_command('L', 'I', 'S', '2');
constexpr std::uint32_t kDentV1 = protocol::make_command('D', 'E', 'N', 'T');
constexpr std::uint32_t kDentV2 = protocol::make_command('D', 'N', 'T', '2');
constexpr std::uint32_t kRecvV1 = protocol::make_command('R', 'E', 'C', 'V');
constexpr std::uint32_t kSendV1 = protocol::make_command('S', 'E', 'N', 'D');
constexpr std::uint32_t kStatV2 = protocol::make_command('S', 'T', 'A', '2');
constexpr std::uint32_t kStatV1 = protocol::make_command('S', 'T', 'A', 'T');
constexpr std::uint32_t kData = protocol::make_command('D', 'A', 'T', 'A');
constexpr std::uint32_t kDone = protocol::make_command('D', 'O', 'N', 'E');
constexpr std::uint32_t kOkay = protocol::make_command('O', 'K', 'A', 'Y');
constexpr std::uint32_t kFail = protocol::make_command('F', 'A', 'I', 'L');
constexpr std::uint32_t kQuit = protocol::make_command('Q', 'U', 'I', 'T');

// The v2 RECV/SEND requests carry a compression flag in a setup packet that
// follows the path request, and only their `DATA` payloads are compressed. The ids
// are the little-endian encoding of four ASCII characters, like the v1 ids above.
constexpr std::uint32_t kRecvV2 = protocol::make_command('R', 'C', 'V', '2');
constexpr std::uint32_t kSendV2 = protocol::make_command('S', 'N', 'D', '2');
// `kSyncFlagZstd` selects zstd, from `SyncFlag` in `file_sync_protocol.h`.
constexpr std::uint32_t kSyncFlagZstd = 4;

// The device's `features=` names for the v2 forms. `sendrecv_v2` selects the v2
// requests and `sendrecv_v2_zstd` the codec, exactly like adb. The match is
// exact, like `ls_v2`, because a substring search would treat `sendrecv_v2` as
// matching `recv_v2`.
constexpr std::string_view kSendRecvV2Feature = "sendrecv_v2";
constexpr std::string_view kSendRecvV2ZstdFeature = "sendrecv_v2_zstd";

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

// A STAT body follows its id. v1 is `mode`, `size`, `mtime`; v2 is the v2 DENT
// body without the trailing name, so it inserts an error before the metadata.
constexpr std::size_t kStatV1BodySize = 12;
constexpr std::size_t kStatV2BodySize = 68;

// Field offsets within a DENT or STAT body, named after the layouts above so the
// parsing reads as fields rather than numbers. The v2 offsets are shared by the v2
// DENT and STAT forms, which differ only in the trailing name.
constexpr std::size_t kErrorOffset = 0;
constexpr std::size_t kDentV1ModeOffset = 0;
constexpr std::size_t kDentV1SizeOffset = 4;
constexpr std::size_t kDentV1MtimeOffset = 8;
constexpr std::size_t kDentV2ModeOffset = 20;
constexpr std::size_t kDentV2SizeOffset = 36;
constexpr std::size_t kDentV2MtimeOffset = 52;

// The `ls_v2` feature selects the v2 DENT form and `stat_v2` the v2 STAT form,
// exactly like adb.
constexpr std::string_view kLsV2Feature = "ls_v2";
constexpr std::string_view kStatV2Feature = "stat_v2";

// A RECV transfer arrives as `DATA` chunks of at most this many bytes, which is
// `SYNC_DATA_MAX` in `file_sync_protocol.h`. The daemon never exceeds it, so a
// larger size means the stream is not what it claims to be.
constexpr std::uint32_t kMaxChunkSize = 64 * 1024;

// The 32-bit little-endian helpers are shared with the ADB header and shell_v2.
using protocol::read_u32_le;
using protocol::write_u32_le;

std::uint64_t read_u64_le(const std::byte *in) noexcept
{
    return static_cast<std::uint64_t>(read_u32_le(in)) | (static_cast<std::uint64_t>(read_u32_le(in + 4)) << 32);
}

// Writes a request that carries a path: the id, the path length, and the path
// itself, which is not null-terminated. A sync request is not an ADB message, so it
// has no 24-byte header.
Status write_path_request(Stream &stream, std::uint32_t id, std::string_view path)
{
    std::vector<std::byte> request(kRequestSize + path.size());
    write_u32_le(request.data(), id);
    write_u32_le(request.data() + 4, static_cast<std::uint32_t>(path.size()));
    std::copy_n(reinterpret_cast<const std::byte *>(path.data()), path.size(), request.data() + kRequestSize);
    return stream.write(request);
}

// FAIL is `sync_status { id, msglen }` followed by the reason. The daemon sends it
// when the request itself is rejected, for example when the path is too long or the
// file cannot be opened. Reading the reason can itself fail, so the returned `Error`
// is either the device's reason or the transport's.
Error sync_fail(Stream &stream)
{
    std::array<std::byte, 4> length_bytes{};
    if (const auto read = stream.read(length_bytes); !read)
    {
        return read.error();
    }
    const std::uint32_t length = read_u32_le(length_bytes.data());
    std::vector<std::byte> reason(length);
    if (const auto read = stream.read(reason); !read)
    {
        return read.error();
    }
    return Error{ErrorCode::Device, "the sync request failed: " +
                                        std::string(reinterpret_cast<const char *>(reason.data()), reason.size())};
}

// Writes the `QUIT` request, which leaves sync mode; the daemon then closes the
// stream.
Status write_quit(Stream &stream)
{
    std::array<std::byte, kRequestSize> quit{};
    write_u32_le(quit.data(), kQuit);
    return stream.write(quit);
}

#if defined(ADBCPP_HAS_COMPRESSION)
// A v2 request is the path request followed by a setup packet, and adb sends
// both in one write. RECV's setup is `sync_recv_v2 { id, flags }`; SEND's is
// `sync_send_v2 { id, mode, flags }`, where the mode is the value the v1 form
// would carry after the path's comma.
Status write_recv_v2(Stream &stream, std::string_view path)
{
    std::vector<std::byte> request(kRequestSize + path.size() + 8);
    write_u32_le(request.data(), kRecvV2);
    write_u32_le(request.data() + 4, static_cast<std::uint32_t>(path.size()));
    std::copy_n(reinterpret_cast<const std::byte *>(path.data()), path.size(), request.data() + kRequestSize);
    write_u32_le(request.data() + kRequestSize + path.size(), kRecvV2);
    write_u32_le(request.data() + kRequestSize + path.size() + 4, kSyncFlagZstd);
    return stream.write(request);
}

Status write_send_v2(Stream &stream, std::string_view path, std::uint32_t mode)
{
    std::vector<std::byte> request(kRequestSize + path.size() + 12);
    write_u32_le(request.data(), kSendV2);
    write_u32_le(request.data() + 4, static_cast<std::uint32_t>(path.size()));
    std::copy_n(reinterpret_cast<const std::byte *>(path.data()), path.size(), request.data() + kRequestSize);
    std::byte *setup = request.data() + kRequestSize + path.size();
    write_u32_le(setup, kSendV2);
    write_u32_le(setup + 4, mode);
    write_u32_le(setup + 8, kSyncFlagZstd);
    return stream.write(request);
}
#endif

// Reads the device's reply to a request it acknowledges with `OKAY`, or rejects
// with `FAIL` and a reason. The reply is `sync_status { id, msglen }`; only `FAIL`
// has a message behind it.
Status read_status(Stream &stream)
{
    std::array<std::byte, 4> id_bytes{};
    if (const auto read = stream.read(id_bytes); !read)
    {
        return tl::unexpected(read.error());
    }
    const std::uint32_t id = read_u32_le(id_bytes.data());

    if (id == kFail)
    {
        return tl::unexpected(sync_fail(stream));
    }
    if (id != kOkay)
    {
        return tl::unexpected(Error{ErrorCode::Protocol, "unexpected sync response"});
    }

    // The acknowledged message length is zero and is ignored.
    std::array<std::byte, 4> length_bytes{};
    return stream.read(length_bytes);
}

// The device applies the final `DONE`'s size as the file's modification time, so
// the local file's is converted to seconds since the epoch. A
// `std::filesystem::file_time_type` is not a Unix time and its epoch differs by
// platform, so the offset between the file clock and the system clock is measured
// once and applied to the file's time.
std::int64_t local_mtime(const std::filesystem::path &path)
{
    std::error_code error;
    const auto file_time = std::filesystem::last_write_time(path, error);
    if (error)
    {
        return std::time(nullptr);
    }

    const auto file_now = std::filesystem::file_time_type::clock::now();
    const auto system_now = std::chrono::system_clock::now();
    const auto offset = std::chrono::duration_cast<std::chrono::seconds>(system_now.time_since_epoch()) -
                        std::chrono::duration_cast<std::chrono::seconds>(file_now.time_since_epoch());
    const auto mtime = std::chrono::duration_cast<std::chrono::seconds>(file_time.time_since_epoch()) + offset;
    return std::chrono::system_clock::to_time_t(std::chrono::system_clock::time_point(mtime));
}

// The mode sent with `SEND` is the local file's, so the device creates the file
// with the same permissions.
std::uint32_t local_mode(const std::filesystem::path &path)
{
    // S_IFREG, because the file being pushed is always a regular file.
    std::uint32_t mode = 0100000u;

#if defined(_WIN32)
    // Windows has no POSIX mode and reports only whether a file is read-only, so
    // either 0644 or 0444 is used, like adb's own Windows support.
    std::error_code error;
    const auto permissions = std::filesystem::status(path, error).permissions();
    if (error || (permissions & std::filesystem::perms::owner_write) == std::filesystem::perms::none)
    {
        return mode | 0444u;
    }
    return mode | 0644u;
#else
    struct stat info{};
    if (::stat(path.c_str(), &info) != 0)
    {
        return mode | 0600u;
    }
    // The daemon copies the user bits to the group and other bits, so only the
    // permission bits are needed here.
    return static_cast<std::uint32_t>(info.st_mode & 0777u);
#endif
}

#if defined(ADBCPP_HAS_COMPRESSION)
// The compressed form of a `SYNC_DATA_MAX` chunk is at most `ZSTD_compressBound`,
// which is what the encoder cannot exceed. It is larger than the chunk itself
// because a frame adds its own header, so the compressed size is checked against
// this and not against `kMaxChunkSize`.
const std::size_t kMaxCompressedChunkSize = ZSTD_compressBound(kMaxChunkSize);

// The v2 RECV form: the device streams one zstd frame across `DATA` chunks, so a
// chunk boundary is not a frame boundary and each chunk is fed to the decoder
// rather than written as it arrives.
Status pull_v2(Connection &connection, std::string_view remote_path, const std::filesystem::path &local_path)
{
    auto stream = Stream::open(connection, "sync:");
    if (!stream)
    {
        return tl::unexpected(stream.error());
    }

    if (const auto written = write_recv_v2(*stream, remote_path); !written)
    {
        return tl::unexpected(written.error());
    }

    std::ofstream output(local_path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        return tl::unexpected(Error{ErrorCode::Io, "cannot open the local file for writing"});
    }

    ZstdDecoder decoder;
    while (true)
    {
        // Every response starts with a four-byte id.
        std::array<std::byte, 4> id_bytes{};
        if (const auto read = stream->read(id_bytes); !read)
        {
            return tl::unexpected(read.error());
        }
        const std::uint32_t id = read_u32_le(id_bytes.data());

        if (id == kFail)
        {
            return tl::unexpected(sync_fail(*stream));
        }

        std::array<std::byte, 4> size_bytes{};
        if (const auto read = stream->read(size_bytes); !read)
        {
            return tl::unexpected(read.error());
        }
        const std::uint32_t size = read_u32_le(size_bytes.data());

        if (id == kDone)
        {
            break;
        }

        if (id != kData)
        {
            return tl::unexpected(Error{ErrorCode::Protocol, "unexpected sync response"});
        }

        if (size > kMaxCompressedChunkSize)
        {
            return tl::unexpected(Error{ErrorCode::Protocol, "the sync chunk is larger than the compression bound"});
        }

        std::vector<std::byte> chunk(size);
        if (const auto read = stream->read(chunk); !read)
        {
            return tl::unexpected(read.error());
        }

        // The decoded bytes are written as they arrive, so the file is never held
        // in memory whole.
        auto decoded = decoder.decode(chunk);
        if (!decoded)
        {
            return tl::unexpected(decoded.error());
        }
        output.write(reinterpret_cast<const char *>(decoded->data()), static_cast<std::streamsize>(decoded->size()));
        if (!output)
        {
            return tl::unexpected(Error{ErrorCode::Io, "failed to write the local file"});
        }
    }

    return write_quit(*stream);
}

// The v2 SEND form: each chunk is compressed as a complete frame and the
// device's streaming decoder starts the next frame at the end of one.
Status push_v2(Connection &connection, const std::filesystem::path &local_path, std::string_view destination,
               std::uint32_t mode)
{
    auto stream = Stream::open(connection, "sync:");
    if (!stream)
    {
        return tl::unexpected(stream.error());
    }
    if (const auto written = write_send_v2(*stream, destination, mode); !written)
    {
        return tl::unexpected(written.error());
    }

    std::ifstream input(local_path, std::ios::binary);
    if (!input)
    {
        return tl::unexpected(Error{ErrorCode::Io, "cannot open the local file"});
    }

    std::vector<char> buffer(kMaxChunkSize);
    while (input)
    {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count <= 0)
        {
            break;
        }

        auto compressed = compress_zstd(
            std::span(reinterpret_cast<const std::byte *>(buffer.data()), static_cast<std::size_t>(count)));
        if (!compressed)
        {
            return tl::unexpected(compressed.error());
        }

        // The DATA header and its compressed chunk go in one write, like the v1
        // form, so a large file is not one `WRTE` message per chunk.
        std::vector<std::byte> block(8 + compressed->size());
        write_u32_le(block.data(), kData);
        write_u32_le(block.data() + 4, static_cast<std::uint32_t>(compressed->size()));
        std::copy(compressed->begin(), compressed->end(), block.begin() + 8);
        if (const auto written = stream->write(block); !written)
        {
            return tl::unexpected(written.error());
        }
    }

    if (!input.eof())
    {
        return tl::unexpected(Error{ErrorCode::Io, "failed to read the local file"});
    }

    std::array<std::byte, 8> done{};
    write_u32_le(done.data(), kDone);
    write_u32_le(done.data() + 4, static_cast<std::uint32_t>(local_mtime(local_path)));
    if (const auto written = stream->write(done); !written)
    {
        return tl::unexpected(written.error());
    }

    if (const auto status = read_status(*stream); !status)
    {
        return tl::unexpected(status.error());
    }

    return write_quit(*stream);
}
#endif

} // namespace

Result<std::vector<DirEntry>> list(Connection &connection, std::string_view path)
{
    if (path.size() > kMaxPathLength)
    {
        return tl::unexpected(
            Error{ErrorCode::InvalidArgument, "the path is longer than the sync limit of 1024 bytes"});
    }

    // `ls_v2` selects the v2 DENT form. Matching adb, it is an exact feature
    // match rather than a substring search.
    const bool v2 = connection.supports_feature(kLsV2Feature);

    // Requesting the `sync:` service puts the stream in sync mode.
    auto stream = Stream::open(connection, "sync:");
    if (!stream)
    {
        return tl::unexpected(stream.error());
    }

    // LIST(id, path_length, "path").
    if (const auto written = write_path_request(*stream, v2 ? kListV2 : kListV1, path); !written)
    {
        return tl::unexpected(written.error());
    }

    const std::size_t body_size = v2 ? kDentV2BodySize : kDentV1BodySize;
    const std::size_t name_length_offset = v2 ? kDentV2NameLengthOffset : kDentV1NameLengthOffset;

    std::vector<DirEntry> entries;
    while (true)
    {
        // Every response starts with a four-byte id.
        std::array<std::byte, 4> id_bytes{};
        if (const auto read = stream->read(id_bytes); !read)
        {
            return tl::unexpected(read.error());
        }
        const std::uint32_t id = read_u32_le(id_bytes.data());

        // The request itself was rejected, for example because the path is too
        // long. Its body is a length and a reason, not a DENT body.
        if (id == kFail)
        {
            return tl::unexpected(sync_fail(*stream));
        }

        // DONE is a full DENT struct whose id is DONE, so its body is read too
        // to keep the stream in sync, and then the listing is over.
        std::vector<std::byte> body(body_size);
        if (const auto read = stream->read(body); !read)
        {
            return tl::unexpected(read.error());
        }
        if (id == kDone)
        {
            break;
        }

        if (id != (v2 ? kDentV2 : kDentV1))
        {
            return tl::unexpected(Error{ErrorCode::Protocol, "unexpected sync response"});
        }

        // The name always follows, even when the entry failed, so it is read
        // first to keep the stream in sync.
        const std::uint32_t name_length = read_u32_le(body.data() + name_length_offset);
        if (name_length > kMaxNameLength)
        {
            return tl::unexpected(Error{ErrorCode::Protocol, "the entry name is longer than 255 bytes"});
        }
        std::vector<std::byte> name(name_length);
        if (const auto read = stream->read(name); !read)
        {
            return tl::unexpected(read.error());
        }

        // v2 reports a failed `lstat` per entry instead of dropping it. The
        // error is the first field of the body.
        if (v2 && read_u32_le(body.data() + kErrorOffset) != 0)
        {
            continue;
        }

        DirEntry entry;
        entry.name.assign(reinterpret_cast<const char *>(name.data()), name.size());
        if (v2)
        {
            entry.mode = read_u32_le(body.data() + kDentV2ModeOffset);
            entry.size = read_u64_le(body.data() + kDentV2SizeOffset);
            entry.mtime = static_cast<std::int64_t>(read_u64_le(body.data() + kDentV2MtimeOffset));
        }
        else
        {
            entry.mode = read_u32_le(body.data() + kDentV1ModeOffset);
            entry.size = read_u32_le(body.data() + kDentV1SizeOffset);
            entry.mtime = read_u32_le(body.data() + kDentV1MtimeOffset);
        }
        entries.push_back(std::move(entry));
    }

    if (const auto quit = write_quit(*stream); !quit)
    {
        return tl::unexpected(quit.error());
    }

    return entries;
}

Status pull(Connection &connection, std::string_view remote_path, const std::filesystem::path &local_path,
            SyncCompression compression)
{
    if (remote_path.size() > kMaxPathLength)
    {
        return tl::unexpected(
            Error{ErrorCode::InvalidArgument, "the path is longer than the sync limit of 1024 bytes"});
    }

#if defined(ADBCPP_HAS_COMPRESSION)
    // The v2 form needs the codec on both sides, so it is used only when the
    // device advertised it. A caller who asked for zstd explicitly gets an error
    // rather than a silent v1 transfer.
    if (compression != SyncCompression::None && connection.supports_feature(kSendRecvV2Feature) &&
        connection.supports_feature(kSendRecvV2ZstdFeature))
    {
        return pull_v2(connection, remote_path, local_path);
    }
#endif
    if (compression == SyncCompression::Zstd)
    {
        return tl::unexpected(
            Error{ErrorCode::InvalidArgument, "zstd compression is not available for this device or build"});
    }

    // Requesting the `sync:` service puts the stream in sync mode.
    auto stream = Stream::open(connection, "sync:");
    if (!stream)
    {
        return tl::unexpected(stream.error());
    }

    // RECV(id, path_length, "path"), the same request layout as LIST. The v1 form
    // is used because the v2 form is only for compression and `compression` did not
    // select it; both send the file as `DATA` chunks.
    if (const auto written = write_path_request(*stream, kRecvV1, remote_path); !written)
    {
        return tl::unexpected(written.error());
    }

    // Create or truncate the local file before the transfer starts, so a failure
    // leaves a partial file rather than a missing one.
    std::ofstream output(local_path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        return tl::unexpected(Error{ErrorCode::Io, "cannot open the local file for writing"});
    }

    while (true)
    {
        // Every response starts with a four-byte id.
        std::array<std::byte, 4> id_bytes{};
        if (const auto read = stream->read(id_bytes); !read)
        {
            return tl::unexpected(read.error());
        }
        const std::uint32_t id = read_u32_le(id_bytes.data());

        if (id == kFail)
        {
            return tl::unexpected(sync_fail(*stream));
        }

        // DATA and DONE are both `sync_data { id, size }`: DATA is followed by
        // `size` bytes, and DONE carries nothing and ends the transfer.
        std::array<std::byte, 4> size_bytes{};
        if (const auto read = stream->read(size_bytes); !read)
        {
            return tl::unexpected(read.error());
        }
        const std::uint32_t size = read_u32_le(size_bytes.data());

        if (id == kDone)
        {
            break;
        }

        if (id != kData)
        {
            return tl::unexpected(Error{ErrorCode::Protocol, "unexpected sync response"});
        }

        // The daemon caps a chunk at SYNC_DATA_MAX, so a larger one is not a
        // chunk of a file.
        if (size > kMaxChunkSize)
        {
            return tl::unexpected(Error{ErrorCode::Protocol, "the sync chunk is larger than 64 KiB"});
        }

        // Each chunk is written as it arrives, so the file is never held in
        // memory whole.
        std::vector<std::byte> chunk(size);
        if (const auto read = stream->read(chunk); !read)
        {
            return tl::unexpected(read.error());
        }
        output.write(reinterpret_cast<const char *>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
        if (!output)
        {
            return tl::unexpected(Error{ErrorCode::Io, "failed to write the local file"});
        }
    }

    return write_quit(*stream);
}

Result<std::optional<FileStat>> stat(Connection &connection, std::string_view path)
{
    if (path.size() > kMaxPathLength)
    {
        return tl::unexpected(
            Error{ErrorCode::InvalidArgument, "the path is longer than the sync limit of 1024 bytes"});
    }

    // `stat_v2` selects the v2 STAT form, which reports an error instead of
    // failing the request. Matching adb, it is an exact feature match.
    const bool v2 = connection.supports_feature(kStatV2Feature);

    auto stream = Stream::open(connection, "sync:");
    if (!stream)
    {
        return tl::unexpected(stream.error());
    }
    if (const auto written = write_path_request(*stream, v2 ? kStatV2 : kStatV1, path); !written)
    {
        return tl::unexpected(written.error());
    }

    // Every response starts with a four-byte id.
    std::array<std::byte, 4> id_bytes{};
    if (const auto read = stream->read(id_bytes); !read)
    {
        return tl::unexpected(read.error());
    }

    FileStat result;
    if (v2)
    {
        // v2 is the v2 DENT body without the name: `error`, `dev`, `ino`,
        // `mode`, `nlink`, `uid`, `gid`, `size`, `atime`, `mtime`, `ctime`.
        std::array<std::byte, kStatV2BodySize> body{};
        if (const auto read = stream->read(body); !read)
        {
            return tl::unexpected(read.error());
        }

        // The device reports a missing path here rather than failing the request.
        if (read_u32_le(body.data() + kErrorOffset) != 0)
        {
            if (const auto quit = write_quit(*stream); !quit)
            {
                return tl::unexpected(quit.error());
            }
            return std::optional<FileStat>{};
        }
        result.mode = read_u32_le(body.data() + kDentV2ModeOffset);
        result.size = read_u64_le(body.data() + kDentV2SizeOffset);
        result.mtime = static_cast<std::int64_t>(read_u64_le(body.data() + kDentV2MtimeOffset));
    }
    else
    {
        // v1 is `mode`, `size`, `mtime`. It has no error field, so a missing
        // path comes back as all zeros and cannot be told from an empty file.
        std::array<std::byte, kStatV1BodySize> body{};
        if (const auto read = stream->read(body); !read)
        {
            return tl::unexpected(read.error());
        }
        if (read_u32_le(body.data() + kDentV1ModeOffset) == 0 && read_u32_le(body.data() + kDentV1SizeOffset) == 0 &&
            read_u32_le(body.data() + kDentV1MtimeOffset) == 0)
        {
            if (const auto quit = write_quit(*stream); !quit)
            {
                return tl::unexpected(quit.error());
            }
            return std::optional<FileStat>{};
        }
        result.mode = read_u32_le(body.data() + kDentV1ModeOffset);
        result.size = read_u32_le(body.data() + kDentV1SizeOffset);
        result.mtime = read_u32_le(body.data() + kDentV1MtimeOffset);
    }

    if (const auto quit = write_quit(*stream); !quit)
    {
        return tl::unexpected(quit.error());
    }
    return std::optional<FileStat>{result};
}

Status push(Connection &connection, const std::filesystem::path &local_path, std::string_view remote_path,
            SyncCompression compression)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(local_path, error))
    {
        return tl::unexpected(Error{ErrorCode::InvalidArgument, "the local file is not a regular file"});
    }

    // A destination that is an existing directory receives the file under its local
    // name, exactly like `adb push local.txt /sdcard/`.
    std::string destination(remote_path);
    const auto existing = stat(connection, remote_path);
    if (!existing)
    {
        return tl::unexpected(existing.error());
    }
    if (*existing && (*existing)->is_directory())
    {
        destination.push_back('/');
        destination += local_path.filename().string();
    }

    const std::uint32_t mode = local_mode(local_path);

#if defined(ADBCPP_HAS_COMPRESSION)
    // The v2 form needs the codec on both sides, exactly as `pull` does.
    if (compression != SyncCompression::None && connection.supports_feature(kSendRecvV2Feature) &&
        connection.supports_feature(kSendRecvV2ZstdFeature))
    {
        return push_v2(connection, local_path, destination, mode);
    }
#endif
    if (compression == SyncCompression::Zstd)
    {
        return tl::unexpected(
            Error{ErrorCode::InvalidArgument, "zstd compression is not available for this device or build"});
    }

    // SEND(id, path_length, "path,mode"). The mode is decimal and includes the
    // file type bits; the daemon parses it with `strtoul(..., 0)` and passes it to
    // `open`, which uses only its permission bits.
    const std::string spec = destination + ',' + std::to_string(mode);

    auto stream = Stream::open(connection, "sync:");
    if (!stream)
    {
        return tl::unexpected(stream.error());
    }
    if (const auto written = write_path_request(*stream, kSendV1, spec); !written)
    {
        return tl::unexpected(written.error());
    }

    // The contents follow as DATA chunks, and DONE ends the transfer with the
    // file's modification time as its size.
    std::ifstream input(local_path, std::ios::binary);
    if (!input)
    {
        return tl::unexpected(Error{ErrorCode::Io, "cannot open the local file"});
    }

    std::vector<char> buffer(kMaxChunkSize);
    while (input)
    {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count <= 0)
        {
            break;
        }

        // The DATA header and its chunk go in one write, like adb's `WriteOrDie`.
        // They would otherwise be two `WRTE` messages, and the device acknowledges
        // each one, so this halves the traffic for a large file.
        std::vector<std::byte> block(8 + static_cast<std::size_t>(count));
        write_u32_le(block.data(), kData);
        write_u32_le(block.data() + 4, static_cast<std::uint32_t>(count));
        std::copy_n(reinterpret_cast<const std::byte *>(buffer.data()), static_cast<std::size_t>(count),
                    block.data() + 8);
        if (const auto written = stream->write(block); !written)
        {
            return tl::unexpected(written.error());
        }
    }

    if (!input.eof())
    {
        return tl::unexpected(Error{ErrorCode::Io, "failed to read the local file"});
    }

    std::array<std::byte, 8> done{};
    write_u32_le(done.data(), kDone);
    write_u32_le(done.data() + 4, static_cast<std::uint32_t>(local_mtime(local_path)));
    if (const auto written = stream->write(done); !written)
    {
        return tl::unexpected(written.error());
    }

    // Unlike a listing or a transfer, the device acknowledges the final DONE with
    // OKAY, or rejects the request with FAIL.
    if (const auto status = read_status(*stream); !status)
    {
        return tl::unexpected(status.error());
    }

    return write_quit(*stream);
}

} // namespace adbcpp
