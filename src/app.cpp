#include "adbcpp/app.hpp"

#include <cstddef>
#include <stdexcept>
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
    try
    {
        run(connection, "rm -f " + shell_quote(path));
    }
    catch (...)
    {
        // The stream is already failing; the install result is what matters.
    }
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

PackageResult install(Connection &connection, const std::filesystem::path &apk, std::string_view options)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(apk, error))
    {
        throw std::runtime_error("adbcpp: the APK is not a regular file");
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
    try
    {
        push(connection, apk, remote);
        const auto result = run(connection, command);
        remove_quietly(connection, remote);
        return package_result(result);
    }
    catch (...)
    {
        remove_quietly(connection, remote);
        throw;
    }
}

PackageResult uninstall(Connection &connection, std::string_view package, bool keep_data)
{
    std::string command = "pm uninstall";
    if (keep_data)
    {
        command += " -k";
    }
    command += ' ';
    command += shell_quote(package);
    return package_result(run(connection, command));
}

} // namespace adbcpp
