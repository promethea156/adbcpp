#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "adbcpp/connection.hpp"
#include "adbcpp/crypto/adb_key.hpp"
#include "adbcpp/shell.hpp"
#include "adbcpp/sync.hpp"
#include "adbcpp/usb/usb_transport.hpp"

// Drives two devices at once, one thread each. The objects share no state, so
// this is the supported model for several devices: one thread per device. Each
// argument names a device as `VID:PID` (hex) or `serial:<serial>`, and two are
// needed, because the WinUSB driver admits a single handle per device, so one device
// cannot be opened twice (blocker 28). With no argument the attached ADB devices
// are listed and the first two are used, which is how two identical devices are told
// apart: each is selected by its own USB serial.
namespace
{

// Runs the whole exchange on one device and returns what it printed, so the two
// threads never write to `std::cout` at the same time. The open and the handshake
// are retried, because a device can reset its USB 3 link around them and stall the
// first write (blocker 29).
adbcpp::Result<std::string> drive(adbcpp::usb::DeviceId id, int index)
{
    const std::string tag = std::to_string(index);

    // Every connection loads its own key, and a `Key` is not thread-safe, so the
    // two threads must not share one.
    const auto key = adbcpp::crypto::Key::load_or_generate();
    if (!key)
    {
        return tl::unexpected(key.error());
    }
    const std::string &public_key_string = key->public_key();
    const auto public_key =
        std::span(reinterpret_cast<const std::byte *>(public_key_string.data()), public_key_string.size());

    std::optional<adbcpp::usb::UsbTransport> transport;
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
        // The connection keeps a pointer to the transport, so the transport is
        // emplaced into the optional before `connect` and never moved again.
        transport.emplace(std::move(*opened));
        auto connected = adbcpp::Connection::connect(*transport, public_key,
                                                     [&key](std::span<const std::byte> token)
                                                     {
                                                         return key->sign(token);
                                                     });
        if (!connected)
        {
            connection = tl::unexpected(connected.error());
            transport.reset();
            continue;
        }
        connection = std::move(*connected);
        break;
    }
    if (!transport)
    {
        return tl::unexpected(connection.error());
    }

    std::string log = "device " + tag + " connected to protocol 0x";
    log += std::to_string(connection->device_version());
    if (!connection->device_serial().empty())
    {
        log += " with serial ";
        log += connection->device_serial();
    }
    log += '\n';

    const auto hello = adbcpp::run(*connection, "echo hello");
    if (!hello)
    {
        return tl::unexpected(hello.error());
    }
    log += "device " + tag + " says: " + hello->output;

    // A file round trip, with a per-device name so two connections to the same
    // device do not collide.
    const auto local = std::filesystem::temp_directory_path() / ("adbcpp_multi_" + tag + ".txt");
    {
        std::ofstream output(local, std::ios::binary | std::ios::trunc);
        output << "adbcpp multi test\n";
    }
    const std::string remote = "/data/local/tmp/adbcpp_multi_" + tag + ".txt";
    if (const auto status = adbcpp::push(*connection, local, remote); !status)
    {
        return tl::unexpected(status.error());
    }
    std::filesystem::remove(local);

    const auto pulled = std::filesystem::temp_directory_path() / ("adbcpp_multi_back_" + tag + ".txt");
    if (const auto status = adbcpp::pull(*connection, remote, pulled); !status)
    {
        return tl::unexpected(status.error());
    }
    std::string contents;
    {
        std::ifstream input(pulled, std::ios::binary);
        contents.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    std::filesystem::remove(pulled);
    (void)adbcpp::run(*connection, "rm -f " + remote);
    if (contents != "adbcpp multi test\n")
    {
        return tl::unexpected(
            adbcpp::Error{adbcpp::ErrorCode::Protocol, "device " + tag + " returned different bytes"});
    }
    log += "device " + tag + " round-tripped a file\n";

    connection->close();
    return log;
}

} // namespace

int main(int argc, char **argv)
{
    std::vector<adbcpp::usb::DeviceId> devices;
    for (int i = 1; i < argc; ++i)
    {
        const auto parsed = adbcpp::usb::DeviceId::parse(argv[i]);
        if (!parsed)
        {
            std::cerr << "error: " << parsed.error().message << '\n';
            return 1;
        }
        devices.push_back(*parsed);
    }

    // With no selector, the attached devices are listed and the first two used.
    // Each is then opened by its own serial, so two identical devices do not both
    // resolve to the first (blocker 28).
    if (devices.empty())
    {
        const auto attached = adbcpp::usb::UsbTransport::list();
        if (!attached)
        {
            std::cerr << "error: " << attached.error().message << '\n';
            return 1;
        }
        if (attached->size() < 2)
        {
            std::cerr << "warning: two ADB devices are needed, but " << attached->size() << " is attached; skipping\n";
            return 0;
        }
        devices.assign(attached->begin(), attached->begin() + 2);
    }
    while (devices.size() < 2)
    {
        devices.push_back(devices.front());
    }

    std::vector<std::string> logs(2);
    std::vector<std::thread> threads;
    threads.reserve(2);
    for (std::size_t i = 0; i < 2; ++i)
    {
        threads.emplace_back(
            [&devices, &logs, i]
            {
                const auto result = drive(devices[i], static_cast<int>(i));
                logs[i] = result ? *result : "error: " + result.error().message + '\n';
            });
    }
    for (auto &thread : threads)
    {
        thread.join();
    }

    for (const auto &log : logs)
    {
        std::cout << log;
    }
    return 0;
}
