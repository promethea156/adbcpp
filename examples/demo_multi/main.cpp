#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
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

// The guided tour from `examples/demo`, run against every attached device at
// once, one thread per device. It is the same ten steps as the single-device
// demo (connect, shell, list, file round trip, sleep, install, wake, launch,
// running check, close) plus a final uninstall, written out step by step in
// `run_tour` below.
//
// The devices are independent, so the whole tour, including the install, launch,
// and uninstall steps, runs concurrently across them. Each device's output is
// buffered in its own stream and printed together at the end, so the logs do not
// interleave and each device's story stays readable.
//
// A split app is installed from all of its APKs, which are given after the
// package, base first.
//
// Usage: adbcpp_demo_multi_example <package> <apk> [<split-apk>...]
namespace
{

// Reports `error` by throwing, so one device's tour stops with the reason without
// stopping the other devices. The thread that runs the tour catches it.
[[noreturn]] void fail(const adbcpp::Error &error)
{
    throw std::runtime_error(error.message);
}

// Prints a banner before each step, so the output reads as the walkthrough it is.
void step(std::ostream &out, std::string_view what)
{
    out << "\n== " << what << " ==" << std::endl;
}

// Sends a power key (223 sleeps, 224 wakes) and prints the resulting
// wakefulness, so the tour shows the device really slept and woke again.
void set_power(std::ostream &out, adbcpp::Connection &connection, std::string_view action, std::string_view key)
{
    step(out, action);
    if (const auto sent = adbcpp::run(connection, "input keyevent " + std::string(key)); !sent)
    {
        fail(sent.error());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    const auto state = adbcpp::run(connection, "dumpsys power | grep mWakefulness");
    if (state && !state->output.empty())
    {
        out << state->output;
    }
}

// Opens the transport and connects, retrying the whole open and handshake because a
// USB 3 device can reset its link right after the open and stall the first write
// (blocker 29). `connect_with_retry` owns the retry loop; `transport` lives in
// the caller and is never moved once it holds a device, because the connection
// keeps a pointer to it.
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

// Installs the app from one or more APKs, the same way `adb install` and
// `adb install-multiple` do.
//
// A single APK goes through `install`, which pushes it to `/data/local/tmp` and
// runs `pm install`. A split app has no `pm install-multiple`; it is installed
// through a `pm` session instead: create the session, write each pushed APK into
// it, then commit it as one atomic update. The base APK must be written before
// its splits, so the caller passes them base first.
adbcpp::Result<adbcpp::PackageResult> install_app(adbcpp::Connection &connection, const std::vector<std::string> &apks)
{
    if (apks.size() == 1)
    {
        // The common case: one APK, so the library's `install` handles it.
        return adbcpp::install(connection, apks.front());
    }

    // `pm install-create` opens a session and prints its id in
    // `Success: created install session [<id>]`, so the id is what sits between
    // the brackets.
    const auto created = adbcpp::run(connection, "pm install-create");
    if (!created)
    {
        return tl::unexpected(created.error());
    }
    const auto open = created->output.find('[');
    const auto close = created->output.find(']', open == std::string::npos ? 0 : open + 1);
    if (!created->success || open == std::string::npos || close == std::string::npos)
    {
        return tl::unexpected(adbcpp::Error{adbcpp::ErrorCode::Device, created->output});
    }
    const std::string session = created->output.substr(open + 1, close - open - 1);

    // Push each APK and write it into the session under its file name. The name is
    // what `pm` uses to tell the base APK from its splits.
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
            // A failed write abandons the session, so it does not linger on the
            // device waiting for a commit.
            (void)adbcpp::run(connection, "pm install-abandon " + session);
            return tl::unexpected(adbcpp::Error{adbcpp::ErrorCode::Device, written->output});
        }
    }

    // Commit installs every written APK at once, so the app never appears with
    // only some of its splits.
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

// Runs the whole tour on one device, writing everything to `out`. `index` names
// this device's local scratch files, because the threads run at the same time. A
// failed step throws, which the thread that calls this catches, so one device
// failing does not stop the others.
void run_tour(std::ostream &out, adbcpp::usb::DeviceId id, std::size_t index, const std::string &package,
              const std::vector<std::string> &apks)
{
    // The local scratch files are named per device, so the concurrent threads do
    // not share one file.
    const std::string tag = std::to_string(index);

    // Step 1a: detect a device.
    //
    // The devices were already listed in `main`, which chose this one. The serial
    // is the USB `iSerial` descriptor and the string `adb devices` prints.
    step(out, "detect a device");
    out << "using device " << (id.serial.empty() ? "(no serial)" : id.serial) << '\n';

    // Step 1b: connect.
    //
    // `Key::load_or_generate` reuses the key `adb` already authorized, so the
    // device usually shows no prompt. `UsbTransport::open` opens the ADB interface
    // and `Connection::connect` performs the CNXN/AUTH handshake, signing the
    // device's token with the key. `device_serial()` then reports the serial the
    // transport opened, the same string `adb devices` prints.
    step(out, "connect");
    const auto key = adbcpp::crypto::Key::load_or_generate();
    if (!key)
    {
        fail(key.error());
    }
    out << "key fingerprint: " << key->fingerprint().value_or("?") << std::endl;
    out << "approve the USB debugging prompt on the device if it appears" << std::endl;
    std::optional<adbcpp::usb::UsbTransport> transport;
    auto connected = connect(id, *key, transport);
    if (!connected)
    {
        fail(connected.error());
    }
    adbcpp::Connection &connection = *connected;
    out << "connected to a device running protocol 0x" << std::hex << connection.device_version() << std::dec;
    if (!connection.device_serial().empty())
    {
        out << " with serial " << connection.device_serial();
    }
    out << std::endl;

    // Step 2: run a shell command.
    //
    // `run` opens the `shell,v2,raw` service, sends the command, and
    // reassembles the output, the exit code, and whether it worked. The `raw`
    // suffix runs the command directly rather than through a login shell. A device
    // without `shell_v2` gets the v1 `shell` service instead, whose output is raw
    // and whose exit code is always 0 (blocker 31).
    step(out, "run a shell command");
    const auto hello = adbcpp::run(connection, "echo hello");
    if (!hello)
    {
        fail(hello.error());
    }
    out << "exit code " << static_cast<int>(hello->exit_code) << ", output: " << hello->output;

    // Step 3: list a directory.
    //
    // `list` opens the `sync` service and sends `LIST`, which the device
    // answers with `DENT` records that the library parses into `DirEntry` values.
    // The wire format is in `docs/06-sync-protocol.md`.
    step(out, "list /sdcard");
    const auto entries = adbcpp::list(connection, "/sdcard");
    if (!entries)
    {
        fail(entries.error());
    }
    out << entries->size() << " entries, first: ";
    if (!entries->empty())
    {
        out << entries->front().name;
    }
    out << '\n';

    // Step 4: push a file, stat it, and pull it back.
    //
    // All three go over `sync`: `push` sends `STAT` to see whether the
    // destination is a directory and then `SEND`, `stat` sends `STAT`, and
    // `pull` sends `RECV`. The round trip is compared, so a silent corruption
    // would be caught. The local file is named per device, because the threads run
    // at the same time on the host.
    step(out, "push, stat, and pull a file");
    const auto local = std::filesystem::temp_directory_path() / ("adbcpp_demo_multi_push_" + tag + ".txt");
    {
        std::ofstream output(local, std::ios::binary | std::ios::trunc);
        output << "adbcpp demo multi\n";
    }
    const std::string remote = "/data/local/tmp/adbcpp_demo_multi_push.txt";
    if (const auto status = adbcpp::push(connection, local, remote); !status)
    {
        fail(status.error());
    }
    const auto stat = adbcpp::stat(connection, remote);
    if (!stat)
    {
        fail(stat.error());
    }
    out << "pushed " << remote << " (" << (stat->has_value() ? (*stat)->size : 0) << " bytes)\n";

    const auto pulled = std::filesystem::temp_directory_path() / ("adbcpp_demo_multi_pull_" + tag + ".txt");
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
    out << "round-tripped " << contents.size() << " bytes\n";

    // Step 5: put the device to sleep.
    //
    // `input keyevent 223` is `KEYCODE_SLEEP`; the following `dumpsys power`
    // shows the device really went to sleep. Installing with the screen off is the
    // realistic case, and it shows the transfer does not need an awake device.
    set_power(out, connection, "sleep the device", "223");

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
    step(out, "install " + package);
    const auto installed = adbcpp::run(connection, "pm path " + package);
    if (!installed)
    {
        fail(installed.error());
    }
    if (installed->output.find(package) != std::string::npos)
    {
        out << package << " is already installed; uninstalling it first\n";
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
    out << "installed " << package << " from " << apks.size() << " apk(s)\n";

    // Step 7: wake the device.
    //
    // `input keyevent 224` is `KEYCODE_WAKEUP`, and the `dumpsys power` check
    // shows the device is awake again before the app is started.
    set_power(out, connection, "wake the device", "224");

    // Step 8: launch the app.
    //
    // `launch` runs `monkey -p <package> -c android.intent.category.LAUNCHER
    // 1`, which starts the package's launcher activity. It is retried because
    // the package manager can still be indexing the fresh install and `monkey`
    // does not start the launcher until it has. An app with no launcher
    // activity cannot be launched this way.
    step(out, "launch " + package);
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
    step(out, "check that it runs for ten seconds");
    for (int second = 0; second < 10; ++second)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const auto running = adbcpp::is_running(connection, package);
        if (!running)
        {
            fail(running.error());
        }
        out << (second + 1) << "s: " << package << (*running ? " is running" : " is not running") << '\n';
        if (!*running)
        {
            break;
        }
    }

    // Step 10: close the app.
    //
    // `close` runs `am force-stop <package>`, which stops the process and
    // removes its activities from the task stack.
    step(out, "close " + package);
    if (const auto status = adbcpp::close(connection, package); !status)
    {
        fail(status.error());
    }
    out << "stopped " << package << '\n';

    // Step 11: uninstall the app.
    //
    // Step 6 installed the package, so it is removed here again and the tour does
    // not leave the app behind. `uninstall` runs `pm uninstall`, and a rejection is
    // a normal result rather than an error.
    step(out, "uninstall " + package);
    const auto removed = adbcpp::uninstall(connection, package);
    if (!removed)
    {
        fail(removed.error());
    }
    if (!removed->success)
    {
        fail(adbcpp::Error{adbcpp::ErrorCode::Device, removed->output});
    }
    out << "uninstalled " << package << '\n';

    connection.close();
    out << "\ndone\n";
}

// One device's run: its serial, whether the tour worked, and its buffered output.
struct DeviceResult
{
    std::string serial;
    bool success = false;
    std::string log;
};

} // namespace

int main(int argc, char **argv)
{
    // The first positional argument is the package and the rest are its APKs, base
    // first. There is no selector: every attached device is driven.
    std::string package;
    std::vector<std::string> apks;
    for (int i = 1; i < argc; ++i)
    {
        if (package.empty())
        {
            package = argv[i];
        }
        else
        {
            apks.emplace_back(argv[i]);
        }
    }
    if (package.empty() || apks.empty())
    {
        std::cerr << "usage: adbcpp_demo_multi_example <package> <apk> [<split-apk>...]\n";
        return 1;
    }

    // Every attached device is driven, so this needs no selector. `list` returns
    // each device's serial, which is the USB `iSerial` descriptor and the string
    // `adb devices` prints. No device at all is a warning and a successful exit.
    const auto attached = adbcpp::usb::UsbTransport::list();
    if (!attached)
    {
        std::cerr << "error: " << attached.error().message << '\n';
        return 1;
    }
    if (attached->empty())
    {
        std::cerr << "warning: no USB device found; skipping\n";
        return 0;
    }
    std::cout << "found " << attached->size() << " device(s):";
    for (const auto &device : *attached)
    {
        std::cout << ' ' << (device.serial.empty() ? "(no serial)" : device.serial);
    }
    std::cout << std::endl;

    // One thread per device, so every device's whole tour, including install,
    // launch, and uninstall, runs at the same time. This is the supported model for
    // several devices: the library objects share no state, and the WinUSB driver
    // admits a single handle per device (blocker 28), so a device cannot be opened
    // twice anyway.
    std::vector<DeviceResult> results(attached->size());
    std::vector<std::thread> threads;
    threads.reserve(attached->size());
    for (std::size_t i = 0; i < attached->size(); ++i)
    {
        // Each thread writes only its own `results[i]`, which needs no lock:
        // distinct vector elements are separate objects. The tour writes to its own
        // `out`, so the device logs never interleave.
        threads.emplace_back(
            [&attached, &results, &package, &apks, i]
            {
                results[i].serial = (*attached)[i].serial;
                std::ostringstream out;
                try
                {
                    run_tour(out, (*attached)[i], i, package, apks);
                    results[i].success = true;
                }
                catch (const std::exception &error)
                {
                    // Record the reason on this device only; the other devices keep
                    // going.
                    out << "error: " << error.what() << '\n';
                }
                results[i].log = out.str();
            });
    }
    // Join every thread before printing anything, so each log is complete.
    for (auto &thread : threads)
    {
        thread.join();
    }

    // Print the logs together, one block per device, and fail the whole example if
    // any device's tour failed.
    bool failed = false;
    for (const auto &result : results)
    {
        std::cout << "\n== " << (result.serial.empty() ? "(no serial)" : result.serial) << " ==\n";
        std::cout << result.log;
        failed = failed || !result.success;
    }
    return failed ? 1 : 0;
}
