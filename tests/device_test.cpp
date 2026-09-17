#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>

#include "adbcpp/app.hpp"
#include "adbcpp/connection.hpp"
#include "adbcpp/crypto/adb_key.hpp"
#include "adbcpp/shell.hpp"
#include "adbcpp/sync.hpp"
#include "adbcpp/usb/usb_transport.hpp"

// The end-to-end integration test: the same flow as `examples/usb`, run against a
// real device. It is a plain executable rather than a Catch2 test so it can exit
// with `kSkip` when no matching USB device is attached.
namespace
{

// CTest reports this exit code as a skipped test.
constexpr int kSkip = 77;

// Reports `error` on stderr, so a failed library call stops the test with the
// reason instead of an unhandled exception.
void report(const adbcpp::Error &error)
{
    std::cerr << "error: " << error.message << '\n';
}

// Slice 5: install and uninstall. The failure paths run always, because they
// touch no package: a file that is not an APK is rejected, and a package that is
// not installed cannot be removed. Both are normal answers from the package
// manager rather than errors, so they arrive as a result and not as an exception.
//
// The two report their failure differently: `pm install` throws a Java exception
// with the reason on its stack trace, so there is no `Failure [...]` to extract,
// while `pm uninstall` prints the usual `Failure [REASON]`.
//
// The success path replaces a package, so it only runs when the caller names a
// disposable APK and its package with `ADBCPP_TEST_APK` and
// `ADBCPP_TEST_PACKAGE`. The APK is pulled first, so it can be put back even if
// the round trip fails part of the way through.
int check_install(adbcpp::Connection &connection)
{
    const auto bogus = std::filesystem::temp_directory_path() / "adbcpp_device_bogus.apk";
    {
        std::ofstream output(bogus, std::ios::binary | std::ios::trunc);
        output << "this is not an APK\n";
    }
    const auto rejected = adbcpp::install(connection, bogus);
    std::filesystem::remove(bogus);

    if (!rejected)
    {
        report(rejected.error());
        return 1;
    }
    if (rejected->success)
    {
        std::cerr << "a file that is not an APK was installed\n";
        return 1;
    }
    if (rejected->output.empty())
    {
        std::cerr << "a rejected APK reported nothing\n";
        return 1;
    }

    const auto missing = adbcpp::uninstall(connection, "com.adbcpp.not.installed");
    if (!missing)
    {
        report(missing.error());
        return 1;
    }
    if (missing->success)
    {
        std::cerr << "a package that is not installed was uninstalled\n";
        return 1;
    }
    if (missing->failure_reason().empty())
    {
        std::cerr << "a rejected package reported no reason: " << missing->output << '\n';
        return 1;
    }

    const char *apk = std::getenv("ADBCPP_TEST_APK");
    const char *package = std::getenv("ADBCPP_TEST_PACKAGE");
    if (apk == nullptr || package == nullptr)
    {
        std::cerr << "ADBCPP_TEST_APK and ADBCPP_TEST_PACKAGE are not set; "
                     "skipping the install and uninstall success path\n";
        return 0;
    }

    const auto saved = std::filesystem::temp_directory_path() / "adbcpp_device_test.apk";
    if (const auto status = adbcpp::pull(connection, apk, saved); !status)
    {
        report(status.error());
        return 1;
    }

    // `-r` replaces the package in place and keeps its data.
    const auto replaced = adbcpp::install(connection, saved, "-r");
    if (!replaced)
    {
        report(replaced.error());
        return 1;
    }
    if (!replaced->success)
    {
        std::cerr << "replacing " << package << " failed: " << replaced->output << '\n';
        return 1;
    }

    const auto removed = adbcpp::uninstall(connection, package);
    if (!removed)
    {
        report(removed.error());
        return 1;
    }
    if (!removed->success)
    {
        std::cerr << "uninstalling " << package << " failed: " << removed->output << '\n';
        return 1;
    }

    const auto gone = adbcpp::run(connection, "pm list packages " + std::string(package));
    if (!gone)
    {
        report(gone.error());
        return 1;
    }
    if (gone->output.find(package) != std::string::npos)
    {
        std::cerr << package << " is still installed after uninstall\n";
        return 1;
    }

    const auto installed = adbcpp::install(connection, saved);
    if (!installed)
    {
        report(installed.error());
        return 1;
    }
    if (!installed->success)
    {
        std::cerr << "installing " << package << " failed: " << installed->output << '\n';
        return 1;
    }

    const auto back = adbcpp::run(connection, "pm path " + std::string(package));
    if (!back)
    {
        report(back.error());
        return 1;
    }
    if (back->output.find(package) == std::string::npos)
    {
        std::cerr << package << " is not installed after install\n";
        return 1;
    }

    std::filesystem::remove(saved);
    return 0;
}

} // namespace

int main()
{
    adbcpp::usb::DeviceId id;
    id.vendor_id = 0x22D9;
    id.product_id = 0x2769;

    const auto present = adbcpp::usb::UsbTransport::is_present(id);
    if (!present)
    {
        report(present.error());
        return 1;
    }
    if (!*present)
    {
        std::cerr << "no USB device " << std::hex << id.vendor_id << ':' << id.product_id << std::dec
                  << " found; skipping\n";
        return kSkip;
    }

    auto transport = adbcpp::usb::UsbTransport::open(id);
    if (!transport)
    {
        report(transport.error());
        return 1;
    }

    const auto key = adbcpp::crypto::Key::load_or_generate();
    if (!key)
    {
        report(key.error());
        return 1;
    }
    const std::string &public_key_string = key->public_key();
    const auto public_key =
        std::span(reinterpret_cast<const std::byte *>(public_key_string.data()), public_key_string.size());

    std::cerr << "connecting; approve the USB debugging prompt on the device "
                 "if it appears\n";
    auto connection = adbcpp::Connection::connect(*transport, public_key,
                                                  [&key](std::span<const std::byte> token)
                                                  {
                                                      return key->sign(token);
                                                  });
    if (!connection)
    {
        report(connection.error());
        return 1;
    }

    const auto result = adbcpp::run(*connection, "echo hello");
    if (!result)
    {
        report(result.error());
        return 1;
    }
    if (result->output != "hello\n")
    {
        std::cerr << "unexpected output: " << result->output << '\n';
        return 1;
    }
    if (result->exit_code != 0)
    {
        std::cerr << "unexpected exit code: " << static_cast<int>(result->exit_code) << '\n';
        return 1;
    }

    // `/` is always present and always holds at least `sdcard`.
    const auto entries = adbcpp::list(*connection, "/");
    if (!entries)
    {
        report(entries.error());
        return 1;
    }
    if (entries->empty())
    {
        std::cerr << "the listing of / is empty\n";
        return 1;
    }

    // Pull a file back: create one in the temporary directory, copy it over
    // sync, and compare the contents. `run` always goes through `sh -c`, so a
    // redirection works.
    const std::string remote = "/data/local/tmp/adbcpp_device_pull_test.txt";
    if (const auto status = adbcpp::run(*connection, "echo adbcpp-pull-test > " + remote); !status)
    {
        report(status.error());
        return 1;
    }

    const auto local = std::filesystem::temp_directory_path() / "adbcpp_device_pull_test.txt";
    if (const auto status = adbcpp::pull(*connection, remote, local); !status)
    {
        report(status.error());
        return 1;
    }

    // The file has to be closed before it can be removed, which Windows
    // enforces.
    std::string contents;
    {
        std::ifstream pulled(local, std::ios::binary);
        contents.assign(std::istreambuf_iterator<char>(pulled), std::istreambuf_iterator<char>());
    }
    if (const auto status = adbcpp::run(*connection, "rm -f " + remote); !status)
    {
        report(status.error());
        return 1;
    }
    std::filesystem::remove(local);

    // Push a file back: create one in the temporary directory, copy it over
    // sync, and pull it back to prove it arrived.
    const auto pushed_local = std::filesystem::temp_directory_path() / "adbcpp_device_push_test.txt";
    {
        std::ofstream output(pushed_local, std::ios::binary | std::ios::trunc);
        output << "adbcpp-push-test\n";
    }

    const std::string pushed_remote = "/data/local/tmp/adbcpp_device_push_test.txt";
    if (const auto status = adbcpp::push(*connection, pushed_local, pushed_remote); !status)
    {
        report(status.error());
        return 1;
    }

    const auto pushed_back = std::filesystem::temp_directory_path() / "adbcpp_device_push_back.txt";
    if (const auto status = adbcpp::pull(*connection, pushed_remote, pushed_back); !status)
    {
        report(status.error());
        return 1;
    }

    std::string pushed_contents;
    {
        std::ifstream input(pushed_back, std::ios::binary);
        pushed_contents.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    if (const auto status = adbcpp::run(*connection, "rm -f " + pushed_remote); !status)
    {
        report(status.error());
        return 1;
    }
    std::filesystem::remove(pushed_local);
    std::filesystem::remove(pushed_back);

    const int install_result = check_install(*connection);

    transport->close();
    if (install_result != 0)
    {
        return install_result;
    }
    if (pushed_contents != "adbcpp-push-test\n")
    {
        std::cerr << "unexpected pushed contents: " << pushed_contents << '\n';
        return 1;
    }
    if (contents != "adbcpp-pull-test\n")
    {
        std::cerr << "unexpected pulled contents: " << contents << '\n';
        return 1;
    }
    return 0;
}
