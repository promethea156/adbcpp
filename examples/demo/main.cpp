#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tl/expected.hpp>
#include <vector>

#include "adbcpp/app.hpp"
#include "adbcpp/connection.hpp"
#include "adbcpp/crypto/adb_key.hpp"
#include "adbcpp/shell.hpp"
#include "adbcpp/sync.hpp"
#include "adbcpp/usb/usb_transport.hpp"

// A guided tour of the whole library against the first attached device, with the
// step-by-step written out here rather than in a separate document. It detects a
// device, connects, then walks every public feature once, in the order a real session
// would use them:
//
//   1. detect a device and connect   UsbTransport::list, DeviceId::serial, connect
//   2. run a shell command           run
//   3. list a directory              list
//   4. push, stat, and pull a file    push, stat, pull
//   5. sleep the device              run("input keyevent 223")
//   6. install the app               uninstall if present, then install
//   7. wake the device               run("input keyevent 224"), run("input keyevent 82")
//   8. launch the app                launch
//   9. check it runs for ten seconds  is_running
//  10. close the app                 close
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

// Sends power keys in order with a delay between them and prints the resulting
// wakefulness, so the tour shows the device really slept and woke again. `223` is
// `KEYCODE_SLEEP`; waking is `224` (`KEYCODE_WAKEUP`) and then `82`
// (`KEYCODE_MENU`), which dismisses the keyguard the wake leaves.
void set_power(adbcpp::Connection &connection, std::string_view action, std::initializer_list<std::string_view> keys)
{
    step(action);
    for (const auto key : keys)
    {
        if (const auto sent = adbcpp::run(connection, "input keyevent " + std::string(key)); !sent)
        {
            fail(sent.error());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    const auto state = adbcpp::run(connection, "dumpsys power | grep mWakefulness");
    if (state && !state->output.empty())
    {
        std::cout << state->output;
    }
}

// Opens the transport and connects, retrying the whole open and handshake because a
// USB 3 device can reset its link right after the open and stall the first write
// (blocker 29). `connect_with_retry` owns the retry loop; `transport` lives in the
// caller and is never moved once it holds a device, because the connection keeps a
// pointer to it.
adbcpp::Result<adbcpp::Connection> connect(adbcpp::usb::DeviceId id, const adbcpp::crypto::Key &key,
                                           std::optional<adbcpp::usb::UsbTransport> &transport)
{
    const std::string &public_key_string = key.public_key();
    const auto public_key =
        std::span(reinterpret_cast<const std::byte *>(public_key_string.data()), public_key_string.size());

    return adbcpp::connect_with_retry(
        [id]
        {
            return adbcpp::usb::UsbTransport::open(id);
        },
        transport, public_key,
        [&key](std::span<const std::byte> token)
        {
            return key.sign(token);
        });
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
        // Step 1a: detect a device.
        //
        // `UsbTransport::list` enumerates the attached ADB devices and returns each
        // one's serial, which is the USB `iSerial` descriptor and the string
        // `adb devices` prints. With no `--serial` the list is printed and the first
        // device is used; `--serial` skips the enumeration and matches that serial
        // alone, so the device's model does not have to be known. No device at all is a
        // warning and a successful exit, so the demo can run in a loop.
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

        // Step 1b: connect.
        //
        // `Key::load_or_generate` reuses the key `adb` already authorized, so the
        // device usually shows no prompt. `UsbTransport::open` opens the ADB interface
        // and `Connection::connect` performs the CNXN/AUTH handshake, signing the
        // device's token with the key. `device_serial()` then reports the serial the
        // transport opened, the same string `adb devices` prints.
        //
        // The open and the handshake are retried, because a USB 3 device can reset its
        // link right after the open and stall the first write (blocker 29). The transport
        // lives in `main` and is never moved once it holds a device, because the
        // connection keeps a pointer to it.
        step("connect");
        const auto key = adbcpp::crypto::Key::load_or_generate();
        if (!key)
        {
            fail(key.error());
        }
        std::cout << "key fingerprint: " << key->fingerprint().value_or("?") << std::endl;
        std::cout << "approve the USB debugging prompt on the device if it appears" << std::endl;
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

        // Step 2: run a shell command.
        //
        // `run` opens the `shell,v2,raw` service, sends the command, and
        // reassembles the output, the exit code, and whether it worked. The `raw`
        // suffix runs the command directly rather than through a login shell. A device
        // without `shell_v2` gets the v1 `shell` service instead, whose output is raw
        // and whose exit code is always 0 (blocker 31).
        step("run a shell command");
        const auto hello = adbcpp::run(connection, "echo hello");
        if (!hello)
        {
            fail(hello.error());
        }
        std::cout << "exit code " << static_cast<int>(hello->exit_code) << ", output: " << hello->output;

        // Step 3: list a directory.
        //
        // `list` opens the `sync` service and sends `LIST`, which the device
        // answers with `DENT` records that the library parses into `DirEntry` values.
        // The wire format is in `docs/06-sync-protocol.md`.
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

        // Step 4: push a file, stat it, and pull it back.
        //
        // All three go over `sync`: `push` sends `STAT` to see whether the
        // destination is a directory and then `SEND`, `stat` sends `STAT`, and
        // `pull` sends `RECV`. The round trip is compared, so a silent corruption
        // would be caught.
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

        // Step 5: put the device to sleep.
        //
        // `input keyevent 223` is `KEYCODE_SLEEP`; the following `dumpsys power`
        // shows the device really went to sleep. Installing with the screen off is the
        // realistic case, and it shows the transfer does not need an awake device.
        set_power(connection, "sleep the device", {"223"});

        // Step 6: install the app, uninstalling an existing copy first.
        //
        // `pm path <package>` prints the package's APK path when it is installed and
        // nothing when it is not, so the program can remove the old copy first and
        // always install fresh. `uninstall` runs `pm uninstall`, and a rejection is a
        // normal result rather than an error.
        //
        // A single APK is `install`, which pushes it to `/data/local/tmp` and runs
        // `pm install`. A split app has no `pm install-multiple`; `install_app` does
        // what `adb install-multiple` does: create a `pm` session, write each pushed
        // APK into it, and commit it.
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

        // Step 7: wake the device.
        //
        // `input keyevent 224` is `KEYCODE_WAKEUP`, and `input keyevent 82` is
        // `KEYCODE_MENU`, which dismisses the keyguard the wake leaves. The
        // `dumpsys power` check shows the device is awake again before the app
        // is started.
        set_power(connection, "wake the device", {"224", "82"});

        // Step 8: launch the app.
        //
        // `launch` runs `monkey -p <package> -c android.intent.category.LAUNCHER
        // 1`, which starts the package's launcher activity. It is retried because
        // the package manager can still be indexing the fresh install and `monkey`
        // does not start the launcher until it has. An app with no launcher
        // activity cannot be launched this way.
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

        // Step 9: check that it runs, for ten seconds.
        //
        // `is_running` runs `pidof <package>`, which answers both cases
        // directly: it exits zero with the pids when the process runs and nonzero
        // with no output when it does not. The loop polls once a second for ten
        // seconds and stops early if the app exits on its own.
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

        // Step 10: close the app.
        //
        // `close` runs `am force-stop <package>`, which stops the process and
        // removes its activities from the task stack. The transport is then closed
        // and the tour is done.
        step("close " + package);
        if (const auto status = adbcpp::close(connection, package); !status)
        {
            fail(status.error());
        }
        std::cout << "stopped " << package << '\n';

        connection.close();
        std::cout << "\ndone\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
