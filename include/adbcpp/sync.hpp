#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "adbcpp/connection.hpp"
#include "adbcpp/error.hpp"
#include "adbcpp/export.hpp"

namespace adbcpp
{

/**
 * @brief A directory entry returned by `list`.
 *
 * The fields are the POSIX metadata the device reports for each entry, the same
 * values `lstat` would give. `mode` is a `st_mode`, so the file type lives in
 * its top bits, exactly like `<sys/stat.h>` on the host.
 */
struct ADBCPP_API DirEntry
{
    /// The entry name, without its directory.
    std::string name;
    /// The POSIX mode: file type and permission bits.
    std::uint32_t mode = 0;
    /// The size in bytes.
    std::uint64_t size = 0;
    /// The last modification time, in seconds since the epoch.
    std::int64_t mtime = 0;

    /// Whether the entry is a directory (`S_ISDIR`).
    bool is_directory() const noexcept
    {
        return (mode & 0170000u) == 0040000u;
    }

    /// Whether the entry is a regular file (`S_ISREG`).
    bool is_regular() const noexcept
    {
        return (mode & 0170000u) == 0100000u;
    }
};

/**
 * @brief A file's metadata, as reported by `stat`.
 *
 * The fields are the POSIX metadata the device reports, the same values `stat`
 * would give on the device. `mode` is a `st_mode`, so the file type lives in its
 * top bits, exactly like `<sys/stat.h>` on the host.
 */
struct ADBCPP_API FileStat
{
    /// The POSIX mode: file type and permission bits.
    std::uint32_t mode = 0;
    /// The size in bytes.
    std::uint64_t size = 0;
    /// The last modification time, in seconds since the epoch.
    std::int64_t mtime = 0;

    /// Whether the path is a directory (`S_ISDIR`).
    bool is_directory() const noexcept
    {
        return (mode & 0170000u) == 0040000u;
    }

    /// Whether the path is a regular file (`S_ISREG`).
    bool is_regular() const noexcept
    {
        return (mode & 0170000u) == 0100000u;
    }
};

/**
 * @brief Lists the entries of a directory on the device.
 *
 * Opens the `sync:` service, sends a LIST request for `path`, and reads the
 * `DENT` entries until the device sends `DONE`. Sync mode is a binary mode that
 * differs from the ADB protocol: a request is an id followed by a length, and the
 * path is not null-terminated. It is documented in AOSP's `docs/dev/sync.md`:
 *
 *   https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/docs/dev/sync.md
 *
 * When the device advertises `ls_v2`, the v2 `DENT` form is used. It carries the
 * full POSIX metadata (`dev`, `ino`, `nlink`, `uid`, `gid`, `atime`, `ctime`) and
 * reports a per-entry error instead of silently dropping entries.
 */
Result<std::vector<DirEntry>> ADBCPP_API list(Connection &connection, std::string_view path);

/**
 * @brief How `pull` and `push` may compress a transfer.
 *
 * The v2 `RECV`/`SEND` forms carry a compression flag, and the v1 forms do not.
 * The codec is zstd, the one adb prefers, and it is built only when
 * `ADBCPP_BUILD_COMPRESSION` is on.
 */
enum class SyncCompression
{
    /// The best codec both sides have, in adb's order (`zstd`, then `lz4`), and
    /// the v1 forms otherwise. This is the default.
    Auto,
    /// The v1 forms, with no compression.
    None,
    /// The v2 forms with zstd. It is an `ErrorCode::InvalidArgument` error when
    /// the build has no zstd codec or the device did not advertise
    /// `sendrecv_v2_zstd`, rather than a silent fallback, so the caller is not
    /// misled.
    Zstd,
    /// The v2 forms with lz4. It is an `ErrorCode::InvalidArgument` error when
    /// the build has no lz4 codec or the device did not advertise
    /// `sendrecv_v2_lz4`, rather than a silent fallback.
    Lz4
};

/**
 * @brief Copies a file from the device to `local_path`.
 *
 * Opens the `sync:` service, sends a RECV request for `remote_path`, and writes each
 * `DATA` chunk to `local_path` as it arrives, so the file is never held in memory.
 * The device ends the transfer with `DONE`. A request the device rejects arrives as a
 * `FAIL` message and is returned as an `ErrorCode::Device` error carrying the
 * daemon's reason.
 *
 * `compression` selects the request form. `Auto` uses the v2 form with zstd when the
 * device advertised `sendrecv_v2_zstd` and the build has the codec, and the v1 form
 * otherwise, so a caller who does not care gets the best available. `None` always
 * uses the v1 form. `Zstd` requires the device to have advertised it.
 *
 * `local_path` is created if it does not exist and truncated if it does, and its
 * parent directory must already exist. A transfer that fails part way through leaves
 * the partial file behind, exactly as adb leaves it, so the caller can decide whether
 * to retry or remove it.
 */
Status ADBCPP_API pull(Connection &connection, std::string_view remote_path, const std::filesystem::path &local_path,
                       SyncCompression compression = SyncCompression::Auto);

/**
 * @brief Stats a path on the device, following symbolic links.
 *
 * Opens the `sync:` service and sends a STAT request for `path`. The device
 * reports a missing path as an error in its response rather than failing the
 * request, so that case is an empty `optional`, not an `Error`.
 *
 * When the device advertises `stat_v2`, the v2 form is used; otherwise the v1
 * form is, which has no error field and reports a missing path as all zeros.
 */
Result<std::optional<FileStat>> ADBCPP_API stat(Connection &connection, std::string_view path);

/**
 * @brief Copies a local file to `remote_path` on the device.
 *
 * Opens the `sync:` service, stats `remote_path`, and sends a SEND request
 * followed by the file's contents as `DATA` chunks of at most 64 KiB. The device
 * acknowledges the last `DONE` with `OKAY`, or rejects the request with `FAIL`,
 * which is returned as an `ErrorCode::Device` error carrying the daemon's reason.
 *
 * The device creates `remote_path`, or overwrites it if it already exists. When
 * `remote_path` names an existing directory, the file is created inside it under
 * `local_path`'s file name, exactly like `adb push local.txt /sdcard/`. The device
 * creates the file with `local_path`'s permissions and modification time, so far as
 * the platform reports them.
 *
 * `compression` selects the request form, exactly as it does for `pull`.
 */
Status ADBCPP_API push(Connection &connection, const std::filesystem::path &local_path, std::string_view remote_path,
                       SyncCompression compression = SyncCompression::Auto);

} // namespace adbcpp
