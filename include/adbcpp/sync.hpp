#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "adbcpp/connection.hpp"
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
std::vector<DirEntry> ADBCPP_API list(Connection &connection, std::string_view path);

/**
 * @brief Copies a file from the device to `local_path`.
 *
 * Opens the `sync:` service, sends a RECV request for `remote_path`, and writes each
 * `DATA` chunk to `local_path` as it arrives, so the file is never held in memory.
 * The device ends the transfer with `DONE`. A request the device rejects arrives as a
 * `FAIL` message and is thrown as a `std::runtime_error`.
 *
 * `local_path` is created if it does not exist and truncated if it does, and its
 * parent directory must already exist. A transfer that fails part way through leaves
 * the partial file behind, exactly as adb leaves it, so the caller can decide whether
 * to retry or remove it.
 */
void ADBCPP_API pull(Connection &connection, std::string_view remote_path, const std::filesystem::path &local_path);

/**
 * @brief Stats a path on the device, following symbolic links.
 *
 * Opens the `sync:` service and sends a STAT request for `path`. The device
 * reports a missing path as an error in its response rather than failing the
 * request, so that case is returned as `std::nullopt` instead of throwing.
 *
 * When the device advertises `stat_v2`, the v2 form is used; otherwise the v1
 * form is, which has no error field and reports a missing path as all zeros.
 */
std::optional<FileStat> ADBCPP_API stat(Connection &connection, std::string_view path);

/**
 * @brief Copies a local file to `remote_path` on the device.
 *
 * Opens the `sync:` service, stats `remote_path`, and sends a SEND request
 * followed by the file's contents as `DATA` chunks of at most 64 KiB. The device
 * acknowledges the last `DONE` with `OKAY`, or rejects the request with `FAIL`,
 * which is thrown as a `std::runtime_error`.
 *
 * The device creates `remote_path`, or overwrites it if it already exists. When
 * `remote_path` names an existing directory, the file is created inside it under
 * `local_path`'s file name, exactly like `adb push local.txt /sdcard/`. The device
 * creates the file with `local_path`'s permissions and modification time, so far as
 * the platform reports them.
 */
void ADBCPP_API push(Connection &connection, const std::filesystem::path &local_path, std::string_view remote_path);

} // namespace adbcpp
