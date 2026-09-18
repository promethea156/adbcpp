#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "adbcpp/app.hpp"
#include "adbcpp/connection.hpp"
#include "adbcpp/crypto/adb_key.hpp"
#include "adbcpp/shell.hpp"
#include "adbcpp/sync.hpp"
#include "adbcpp/usb/usb_transport.hpp"

namespace
{

// Reports `error` and exits, so a failed `Result` from the library stops the
// example with the reason instead of being ignored.
[[noreturn]] void fail(const adbcpp::Error &error)
{
    std::cerr << "error: " << error.message << '\n';
    std::exit(1);
}

} // namespace

// A session over USB: open the ADB interface, authenticate with the key shared
// with adb, then either list a directory (`--list <path>`), pull a file
// (`--pull <remote> <local>`), push a file (`--push <local> <remote>`), install
// an APK (`--install <apk> [options]`), uninstall a package
// (`--uninstall <package>`), launch an app (`--launch <package>`), stop an app
// (`--close <package>`), check whether an app runs (`--running <package>`), or run
// a shell command (the default). All of them mirror what `adb` does, so they are
// interchangeable on the device. `--timeout <ms>` and `--budget <ms>` tune the
// transfer timing anywhere on the command line.
int main(int argc, char **argv)
{
    // `--timeout <ms>` is the timeout for each bulk transfer and `--budget <ms>`
    // the total a transfer waits before giving up. The timeout is short so that a
    // silent device is noticed quickly; the budget is what lets a slow device, or a
    // user approving the debugging prompt, take its time.
    unsigned int transfer_timeout_ms = adbcpp::usb::UsbTransport::kDefaultTransferTimeoutMs;
    unsigned int transfer_budget_ms = adbcpp::usb::UsbTransport::kDefaultTransferBudgetMs;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
    {
        if (std::string_view(argv[i]) == "--timeout" && i + 1 < argc)
        {
            transfer_timeout_ms = static_cast<unsigned int>(std::stoul(argv[++i]));
        }
        else if (std::string_view(argv[i]) == "--budget" && i + 1 < argc)
        {
            transfer_budget_ms = static_cast<unsigned int>(std::stoul(argv[++i]));
        }
        else
        {
            args.emplace_back(argv[i]);
        }
    }

    // The vendor/product id of the target device. Every Android device exposes its
    // ADB function with these particular ids.
    adbcpp::usb::DeviceId id;
    id.vendor_id = 0x22D9;
    id.product_id = 0x2769;

    // Opening a device that is not attached fails, and the ADB interface may be
    // claimed by a running adb server, so check for it first (blocker 5).
    const auto present = adbcpp::usb::UsbTransport::is_present(id);
    if (!present)
    {
        fail(present.error());
    }
    if (!*present)
    {
        std::cerr << "warning: no USB device " << std::hex << id.vendor_id << ':' << id.product_id << std::dec
                  << " found; skipping\n";
        return 0;
    }

    try
    {
        auto transport = adbcpp::usb::UsbTransport::open(id, transfer_timeout_ms, transfer_budget_ms);
        if (!transport)
        {
            fail(transport.error());
        }

        // Load the key adb already authorized, so the device does not prompt. The
        // public key is only needed for the AUTH type 3 fallback (blocker 11).
        const auto key = adbcpp::crypto::Key::load_or_generate();
        if (!key)
        {
            fail(key.error());
        }
        const std::string &public_key_string = key->public_key();
        const auto public_key =
            std::span(reinterpret_cast<const std::byte *>(public_key_string.data()), public_key_string.size());

        const auto fingerprint = key->fingerprint();
        if (!fingerprint)
        {
            fail(fingerprint.error());
        }
        std::cerr << "key fingerprint: " << *fingerprint << '\n';
        std::cerr << "connecting; approve the USB debugging prompt on the device "
                     "if it appears\n";
        auto connection = adbcpp::Connection::connect(*transport, public_key,
                                                      [&key](std::span<const std::byte> token)
                                                      {
                                                          return key->sign(token);
                                                      });
        if (!connection)
        {
            fail(connection.error());
        }
        if (connection->requested_authorization())
        {
            std::cerr << "the device rejected the signature and requested "
                         "authorization\n";
        }
        std::cout << "connected to a device running protocol 0x" << std::hex << connection->device_version() << std::dec
                  << '\n';

        if (args.size() > 1 && args[0] == "--list")
        {
            const auto entries = adbcpp::list(*connection, args[1]);
            if (!entries)
            {
                fail(entries.error());
            }
            for (const auto &entry : *entries)
            {
                std::cout << (entry.is_directory() ? 'd' : '-') << ' ' << entry.size << ' ' << entry.name << '\n';
            }
            transport->close();
            return 0;
        }

        if (args.size() > 2 && args[0] == "--pull")
        {
            if (const auto status = adbcpp::pull(*connection, args[1], args[2]); !status)
            {
                fail(status.error());
            }
            std::cout << "pulled " << args[1] << " to " << args[2] << '\n';
            transport->close();
            return 0;
        }

        if (args.size() > 2 && args[0] == "--push")
        {
            if (const auto status = adbcpp::push(*connection, args[1], args[2]); !status)
            {
                fail(status.error());
            }
            std::cout << "pushed " << args[1] << " to " << args[2] << '\n';
            transport->close();
            return 0;
        }

        if (args.size() > 1 && args[0] == "--install")
        {
            // The flags after the APK go to `pm install` as they are, so
            // `--install app.apk -r` reinstalls an existing package.
            const std::string_view options = args.size() > 2 ? std::string_view(args[2]) : std::string_view{};
            const auto result = adbcpp::install(*connection, args[1], options);
            if (!result)
            {
                fail(result.error());
            }
            std::cout << result->output;
            transport->close();
            return result->success ? 0 : 1;
        }

        if (args.size() > 1 && args[0] == "--uninstall")
        {
            const auto result = adbcpp::uninstall(*connection, args[1]);
            if (!result)
            {
                fail(result.error());
            }
            std::cout << result->output;
            transport->close();
            return result->success ? 0 : 1;
        }

        if (args.size() > 1 && args[0] == "--launch")
        {
            const auto result = adbcpp::launch(*connection, args[1]);
            if (!result)
            {
                fail(result.error());
            }
            std::cout << result->output;
            transport->close();
            return result->success ? 0 : 1;
        }

        if (args.size() > 1 && args[0] == "--close")
        {
            if (const auto status = adbcpp::close(*connection, args[1]); !status)
            {
                fail(status.error());
            }
            std::cout << "stopped " << args[1] << '\n';
            transport->close();
            return 0;
        }

        if (args.size() > 1 && args[0] == "--running")
        {
            const auto running = adbcpp::is_running(*connection, args[1]);
            if (!running)
            {
                fail(running.error());
            }
            std::cout << args[1] << (*running ? " is running" : " is not running") << '\n';
            transport->close();
            return 0;
        }

        const std::string command = args.empty() ? "echo hello" : args[0];
        const auto result = adbcpp::run(*connection, command);
        if (!result)
        {
            fail(result.error());
        }
        std::cout << result->output;

        transport->close();
        return result->exit_code;
    }
    catch (const std::exception &error)
    {
        // The only exception left here is `std::stoul` rejecting a malformed
        // `--timeout` or `--budget`; the library reports its own failures as
        // `Error` values.
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
