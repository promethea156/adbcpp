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

/**
 * @brief Launches an application's main activity.
 *
 * Runs `monkey -p <package> -c android.intent.category.LAUNCHER 1` over the
 * shell service. `monkey` starts the package's `CATEGORY_LAUNCHER` activity
 * directly, where a bare `am start <package>` relies on the device's resolver
 * answering the package as a launcher, which some launchers do not.
 *
 * `monkey` injects the launch event and returns; unlike `am start -W` it does not
 * wait for the activity to be resumed, so `success` means the launch was injected,
 * not that the process is already running. A caller that needs the process up should
 * poll `is_running` rather than check it once.
 *
 * A package that cannot be started is a normal outcome rather than an error:
 * `monkey` exits nonzero and prints `No activities found to run` to its output,
 * so it is returned as `success == false` with the output. An `Error` means the
 * stream itself failed.
 */
Result<CommandResult> ADBCPP_API launch(Connection &connection, std::string_view package);

/**
 * @brief Force-stops an application.
 *
 * Runs `am force-stop` over the shell service, which stops the package's process
 * and removes its activities from the task stack.
 *
 * `am force-stop` exits zero even for a package that is not installed and prints
 * nothing, so there is no per-command answer to inspect and this returns `Status`.
 * An `Error` means the stream itself failed.
 */
Status ADBCPP_API close(Connection &connection, std::string_view package);

/**
 * @brief Whether an application is running.
 *
 * Runs `pidof` over the shell service. `pidof` answers both cases directly: it
 * exits zero and prints the process ids when a matching process runs, and exits
 * nonzero with no output when it does not, so "not running" is a definite `false`
 * rather than an absence.
 *
 * `pidof` matches a process name rather than a package name. A process is named
 * after its package by default, so the two usually agree, but an application that
 * renames its process makes this an approximation.
 */
Result<bool> ADBCPP_API is_running(Connection &connection, std::string_view package);

} // namespace adbcpp
