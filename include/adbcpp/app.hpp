#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "adbcpp/connection.hpp"
#include "adbcpp/error.hpp"
#include "adbcpp/export.hpp"
#include "adbcpp/shell.hpp"

namespace adbcpp
{

/**
 * @brief The result of installing or uninstalling a package.
 *
 * The package manager reports its outcome in its output rather than only in its exit
 * code: it prints `Success` and exits zero when it worked, and
 * `Failure [REASON]` and exits nonzero when it did not. `success` covers both, and
 * `failure_reason()` extracts `REASON`.
 *
 * This is a `CommandResult`, so `output`, `exit_code`, and `success` are defined
 * once and the package manager's answer has the same shape as any other command's.
 */
struct ADBCPP_API PackageResult : CommandResult
{
    /// The reason inside `Failure [...]`, or an empty string when there is none.
    std::string failure_reason() const;
};

/**
 * @brief Installs an APK on the device.
 *
 * The APK is pushed into the device's `/data/local/tmp` with `push`, installed
 * from there with `pm install`, and then removed, which is what `adb install`
 * does. Both services are already in the library, so this is composition rather
 * than a new protocol: `push` is the `sync` `SEND` request and `pm install` runs
 * over the shell service.
 *
 * A package manager that rejects the APK is a normal outcome rather than an error,
 * so it is returned as `success == false` with the device's reason. An `Error` is
 * returned only when the transfer or the stream itself fails, or when `apk` is not a
 * regular file.
 *
 * `options` is passed to `pm install` verbatim, so `-r` reinstalls an existing
 * package and keeps its data, `-d` allows a version downgrade, and `-g` grants all
 * runtime permissions. It is not escaped, so it must not come from untrusted input.
 */
Result<PackageResult> ADBCPP_API install(Connection &connection, const std::filesystem::path &apk,
                                         std::string_view options = {});

/**
 * @brief Uninstalls a package from the device.
 *
 * Runs `pm uninstall` over the shell service. A package the device cannot remove
 * is reported the same way as a rejected install: as `success == false` with the
 * reason, not as an error.
 *
 * `keep_data` adds `-k`, which keeps the package's data and cache directories.
 */
Result<PackageResult> ADBCPP_API uninstall(Connection &connection, std::string_view package, bool keep_data = false);

} // namespace adbcpp
