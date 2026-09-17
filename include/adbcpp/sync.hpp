#pragma once

#include <cstdint>
#include <filesystem>
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

} // namespace adbcpp
