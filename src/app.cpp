#include "adbcpp/app.hpp"

#include <cstddef>
#include <string>
#include <string_view>

#include "adbcpp/shell.hpp"
#include "adbcpp/sync.hpp"

namespace adbcpp
{
namespace
{

// The device runs the command through `sh -c`, so a path or a package name with a
// space would otherwise be split into several words. A single-quoted string passes
// anything except a single quote through unchanged, and a single quote is closed,
// escaped, and reopened, which is the form adb's own `escape_arg` produces.
std::string shell_quote(std::string_view text)
{
    std::string quoted;
    quoted.reserve(text.size() + 2);
    quoted.push_back('\'');
    for (const char character : text)
    {
        if (character == '\'')
        {
            quoted += "'\\''";
        }
        else
        {
            quoted.push_back(character);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

// `pm` prints `Success` and exits zero when it worked, and `Failure [REASON]` and
// exits nonzero when it did not. The exit code alone is not enough, because a
// command the device cannot run at all also exits nonzero.
PackageResult package_result(const CommandResult &result)
{
    PackageResult package;
    package.output = result.output;
    package.exit_code = result.exit_code;
    package.success = result.exit_code == 0 && result.output.find("Success") != std::string::npos;
    return package;
}

// Removes the pushed APK, ignoring a failure: the install result is what the
// caller needs, and a file left in `/data/local/tmp` is harmless.
void remove_quietly(Connection &connection, std::string_view path) noexcept
{
    // The stream may already be failing, and the install result is what matters, so
    // the returned error is deliberately dropped.
    (void)run(connection, "rm -f " + shell_quote(path));
}

} // namespace

std::string PackageResult::failure_reason() const
{
    const std::string prefix = "Failure [";
    const auto start = output.find(prefix);
    if (start == std::string::npos)
    {
        return {};
    }

    const auto reason_start = start + prefix.size();
    const auto end = output.find(']', reason_start);
    if (end == std::string::npos)
    {
        return output.substr(reason_start);
    }
    return output.substr(reason_start, end - reason_start);
}

Result<PackageResult> install(Connection &connection, const std::filesystem::path &apk, std::string_view options)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(apk, error))
    {
        return tl::unexpected(Error{ErrorCode::InvalidArgument, "the APK is not a regular file"});
    }

    // `/data/local/tmp` is the device's scratch directory: the shell user can
    // write to it and the package manager can read from it. `push` appends the
    // local name when the destination is a directory, exactly like
    // `adb push app.apk /data/local/tmp/`.
    const std::string remote = "/data/local/tmp/" + apk.filename().string();

    // `pm install` takes the path as a positional argument, and `options` are the
    // flags that come before it.
    std::string command = "pm install";
    if (!options.empty())
    {
        command += ' ';
        command += options;
    }
    command += ' ';
    command += shell_quote(remote);

    // The pushed APK is removed whether the install worked or not, so a rejected
    // APK is not left behind on the device.
    if (const auto pushed = push(connection, apk, remote); !pushed)
    {
        remove_quietly(connection, remote);
        return tl::unexpected(pushed.error());
    }

    const auto result = run(connection, command);
    remove_quietly(connection, remote);
    if (!result)
    {
        return tl::unexpected(result.error());
    }
    return package_result(*result);
}

Result<PackageResult> uninstall(Connection &connection, std::string_view package, bool keep_data)
{
    std::string command = "pm uninstall";
    if (keep_data)
    {
        command += " -k";
    }
    command += ' ';
    command += shell_quote(package);

    const auto result = run(connection, command);
    if (!result)
    {
        return tl::unexpected(result.error());
    }
    return package_result(*result);
}

Result<CommandResult> launch(Connection &connection, std::string_view package)
{
    // `am start` treats a bare positional argument as a package name and turns it
    // into `ACTION_MAIN`/`CATEGORY_LAUNCHER` with that package, so the device
    // resolves the package's launcher activity. `-W` waits for the launch, so the
    // activity is up by the time a caller checks `is_running`.
    return run(connection, "am start -W " + shell_quote(package));
}

Status close(Connection &connection, std::string_view package)
{
    const auto result = run(connection, "am force-stop " + shell_quote(package));
    if (!result)
    {
        return tl::unexpected(result.error());
    }
    // `am force-stop` exits zero even for a package that is not installed and
    // prints nothing, so its exit code carries no answer to inspect.
    return {};
}

Result<bool> is_running(Connection &connection, std::string_view package)
{
    const auto result = run(connection, "pidof " + shell_quote(package));
    if (!result)
    {
        return tl::unexpected(result.error());
    }
    // `pidof` exits zero with the pids when the process runs, and nonzero with
    // no output when it does not, so the exit code is the whole answer.
    return result->success;
}

} // namespace adbcpp
