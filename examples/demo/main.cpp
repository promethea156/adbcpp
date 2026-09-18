#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "adbcpp/app.hpp"
#include "adbcpp/connection.hpp"
#include "adbcpp/crypto/adb_key.hpp"
#include "adbcpp/shell.hpp"
#include "adbcpp/sync.hpp"
#include "adbcpp/usb/usb_transport.hpp"

// A guided tour of the whole library against the first attached device. It detects a
// device, connects, then walks every public feature once, in the order a real
// session would use them: run a command, list a directory, push and pull a file,
// install an app (uninstalling it first when it is already present), launch it,
// check it stays running for ten seconds, and close it.
//
// A split app is installed from all of its APKs, which are given after the package.
//
// Usage: adbcpp_demo_example <package> <apk> [<split-apk>...] [--serial <serial>]
namespace
{

// Reports `error` and exits, so a failed `Result` stops the tour with the reason
// instead of being ignored.
[[noreturn]] void fail(const adbcpp::Error &error)
{
    std::cerr << "error: " << error.message << '\n';
    std::exit(1);
}

// Prints a banner before each step, so the output reads as the walkthrough it is.
void step(std::string_view what)
{
    std::cout << "\n== " << what << " ==" << std::endl;
}

// Sends a power key (223 sleeps, 224 wakes) and prints the resulting wakefulness, so
// the tour shows the device really slept and woke again.
void set_power(adbcpp::Connection &connection, std::string_view action, std::string_view key)
{
    step(action);
    if (const auto sent = adbcpp::run(connection, "input keyevent " + std::string(key)); !sent)
    {
        fail(sent.error());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    const auto state = adbcpp::run(connection, "dumpsys power | grep mWakefulness");
    if (state && !state->output.empty())
    {
        std::cout << state->output;
    }
}

// Opens the transport and connects, retrying the whole open and handshake because a
// USB 3 device can reset its link right after the open and stall the first write
// (blocker 29). `transport` lives in the caller and is never moved once it holds a
// device, because the connection keeps a pointer to it.
adbcpp::Result<adbcpp::Connection> connect(adbcpp::usb::DeviceId id, const adbcpp::crypto::Key &key,
                                           std::optional<adbcpp::usb::UsbTransport> &transport)
{
    const std::string &public_key_string = key.public_key();
    const auto public_key =
        std::span(reinterpret_cast<const std::byte *>(public_key_string.data()), public_key_string.size());

    adbcpp::Result<adbcpp::Connection> connection =
        tl::unexpected(adbcpp::Error{adbcpp::ErrorCode::Transport, "the device was not opened"});
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        if (attempt > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        auto opened = adbcpp::usb::UsbTransport::open(id);
        if (!opened)
        {
            connection = tl::unexpected(opened.error());
            continue;
        }
        transport.emplace(std::move(*opened));
        auto connected = adbcpp::Connection::connect(*transport, public_key,
                                                     [&key](std::span<const std::byte> token)
                                                     {
                                                         return key.sign(token);
                                                     });
        if (!connected)
        {
            connection = tl::unexpected(connected.error());
            transport.reset();
            continue;
        }
        return std::move(*connected);
    }
    return tl::unexpected(connection.error());
}

// Installs the app from one or more APKs. A single APK is `install`; a split app
// is installed through a `pm` session, which is what `adb install-multiple` does:
// create the session, write each pushed APK into it, then commit it.
adbcpp::Result<adbcpp::PackageResult> install_app(adbcpp::Connection &connection, const std::vector<std::string> &apks)
{
    if (apks.size() == 1)
    {
        return adbcpp::install(connection, apks.front());
    }

    const auto created = adbcpp::run(connection, "pm install-create");
    if (!created)
    {
        return tl::unexpected(created.error());
    }
    // The device answers `Success: created install session [<id>]`.
    const auto open = created->output.find('[');
    const auto close = created->output.find(']', open == std::string::npos ? 0 : open + 1);
    if (!created->success || open == std::string::npos || close == std::string::npos)
    {
        return tl::unexpected(adbcpp::Error{adbcpp::ErrorCode::Device, created->output});
    }
    const std::string session = created->output.substr(open + 1, close - open - 1);

    for (const auto &apk : apks)
    {
        const std::string name = std::filesystem::path(apk).filename().string();
        const std::string remote = "/data/local/tmp/" + name;
        if (const auto status = adbcpp::push(connection, apk, remote); !status)
        {
            return tl::unexpected(status.error());
        }
        const auto written = adbcpp::run(connection, "pm install-write " + session + " " + name + " " + remote);
        if (!written)
        {
            return tl::unexpected(written.error());
        }
        if (!written->success)
        {
            (void)adbcpp::run(connection, "pm install-abandon " + session);
            return tl::unexpected(adbcpp::Error{adbcpp::ErrorCode::Device, written->output});
        }
    }

    const auto committed = adbcpp::run(connection, "pm install-commit " + session);
    if (!committed)
    {
        return tl::unexpected(committed.error());
    }
    adbcpp::PackageResult result;
    result.output = committed->output;
    result.exit_code = committed->exit_code;
    result.success = committed->success;
    return result;
}

} // namespace

int main(int argc, char **argv)
{
    std::string package;
    std::vector<std::string> apks;
    std::optional<std::string> serial;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        if (arg == "--serial" && i + 1 < argc)
        {
            serial = argv[++i];
        }
        else if (package.empty())
        {
            package = arg;
        }
        else
        {
            apks.emplace_back(arg);
        }
    }
    if (package.empty() || apks.empty())
    {
        std::cerr << "usage: adbcpp_demo_example <package> <apk> [<split-apk>...] [--serial <serial>]\n";
        return 1;
    }

    try
    {
        // 1. Detect a device and connect to it.
        step("detect a device");
        adbcpp::usb::DeviceId id;
        if (serial)
        {
            id.serial = *serial;
        }
        else
        {
            const auto devices = adbcpp::usb::UsbTransport::list();
            if (!devices)
            {
                fail(devices.error());
            }
            if (devices->empty())
            {
                std::cerr << "warning: no USB device found; skipping\n";
                return 0;
            }
            std::cout << "found " << devices->size() << " device(s):";
            for (const auto &device : *devices)
            {
                std::cout << ' ' << (device.serial.empty() ? "(no serial)" : device.serial);
            }
            std::cout << std::endl;
            id = devices->front();
        }
        std::cout << "using device " << id.serial << '\n';

        step("connect");
        const auto key = adbcpp::crypto::Key::load_or_generate();
        if (!key)
        {
            fail(key.error());
        }
        std::cout << "key fingerprint: " << key->fingerprint().value_or("?") << std::endl;
        std::cout << "approve the USB debugging prompt on the device if it appears" << std::endl;
        // The transport lives here and is never moved once it holds a device, because
        // the connection keeps a pointer to it.
        std::optional<adbcpp::usb::UsbTransport> transport;
        auto connected = connect(id, *key, transport);
        if (!connected)
        {
            fail(connected.error());
        }
        adbcpp::Connection &connection = *connected;
        std::cout << "connected to a device running protocol 0x" << std::hex << connection.device_version() << std::dec;
        if (!connection.device_serial().empty())
        {
            std::cout << " with serial " << connection.device_serial();
        }
        std::cout << std::endl;

        // 2. Run a shell command and read its output and exit code.
        step("run a shell command");
        const auto hello = adbcpp::run(connection, "echo hello");
        if (!hello)
        {
            fail(hello.error());
        }
        std::cout << "exit code " << static_cast<int>(hello->exit_code) << ", output: " << hello->output;

        // 3. List a directory.
        step("list /sdcard");
        const auto entries = adbcpp::list(connection, "/sdcard");
        if (!entries)
        {
            fail(entries.error());
        }
        std::cout << entries->size() << " entries, first: ";
        if (!entries->empty())
        {
            std::cout << entries->front().name;
        }
        std::cout << '\n';

        // 4. Push a file, stat it, and pull it back.
        step("push, stat, and pull a file");
        const auto local = std::filesystem::temp_directory_path() / "adbcpp_demo_push.txt";
        {
            std::ofstream output(local, std::ios::binary | std::ios::trunc);
            output << "adbcpp demo\n";
        }
        const std::string remote = "/data/local/tmp/adbcpp_demo_push.txt";
        if (const auto status = adbcpp::push(connection, local, remote); !status)
        {
            fail(status.error());
        }
        const auto stat = adbcpp::stat(connection, remote);
        if (!stat)
        {
            fail(stat.error());
        }
        std::cout << "pushed " << remote << " (" << (stat->has_value() ? (*stat)->size : 0) << " bytes)\n";

        const auto pulled = std::filesystem::temp_directory_path() / "adbcpp_demo_pull.txt";
        if (const auto status = adbcpp::pull(connection, remote, pulled); !status)
        {
            fail(status.error());
        }
        std::string contents;
        {
            std::ifstream input(pulled, std::ios::binary);
            contents.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        }
        std::filesystem::remove(local);
        std::filesystem::remove(pulled);
        (void)adbcpp::run(connection, "rm -f " + remote);
        std::cout << "round-tripped " << contents.size() << " bytes\n";

        // 5. Put the device to sleep, then install the app, uninstalling an existing
        // copy first. Installing while the screen is off is the realistic case, and it
        // also shows that the transfer does not need an awake device.
        set_power(connection, "sleep the device", "223");
        step("install " + package);
        const auto installed = adbcpp::run(connection, "pm path " + package);
        if (!installed)
        {
            fail(installed.error());
        }
        if (installed->output.find(package) != std::string::npos)
        {
            std::cout << package << " is already installed; uninstalling it first\n";
            const auto removed = adbcpp::uninstall(connection, package);
            if (!removed)
            {
                fail(removed.error());
            }
            if (!removed->success)
            {
                fail(adbcpp::Error{adbcpp::ErrorCode::Device, removed->output});
            }
        }
        const auto result = install_app(connection, apks);
        if (!result)
        {
            fail(result.error());
        }
        if (!result->success)
        {
            fail(adbcpp::Error{adbcpp::ErrorCode::Device, result->output});
        }
        std::cout << "installed " << package << " from " << apks.size() << " apk(s)\n";

        // 6. Wake the device, then launch the app. Right after a fresh install the
        // package manager can still be indexing, so `am start` may not resolve the
        // launcher yet.
        set_power(connection, "wake the device", "224");
        step("launch " + package);
        adbcpp::Result<adbcpp::CommandResult> launched =
            tl::unexpected(adbcpp::Error{adbcpp::ErrorCode::Device, "the app was not launched"});
        for (int attempt = 0; attempt < 10; ++attempt)
        {
            launched = adbcpp::launch(connection, package);
            if (launched && launched->success)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (!launched)
        {
            fail(launched.error());
        }
        if (!launched->success)
        {
            fail(adbcpp::Error{adbcpp::ErrorCode::Device, launched->output});
        }

        // 7. Check that it is running, and wait ten seconds.
        step("check that it runs for ten seconds");
        for (int second = 0; second < 10; ++second)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const auto running = adbcpp::is_running(connection, package);
            if (!running)
            {
                fail(running.error());
            }
            std::cout << (second + 1) << "s: " << package << (*running ? " is running" : " is not running") << '\n';
            if (!*running)
            {
                break;
            }
        }

        // 8. Close the app.
        step("close " + package);
        if (const auto status = adbcpp::close(connection, package); !status)
        {
            fail(status.error());
        }
        std::cout << "stopped " << package << '\n';

        transport->close();
        std::cout << "\ndone\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
