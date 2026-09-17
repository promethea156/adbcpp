#include <cstddef>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

#include "adbcpp/connection.hpp"
#include "adbcpp/crypto/adb_key.hpp"
#include "adbcpp/shell.hpp"
#include "adbcpp/sync.hpp"
#include "adbcpp/usb/usb_transport.hpp"

// A session over USB: open the ADB interface, authenticate with the key shared
// with adb, then either list a directory (`--list <path>`), pull a file
// (`--pull <remote> <local>`), push a file (`--push <local> <remote>`), or run a
// shell command (the default). All of them mirror what `adb` does, so they are
// interchangeable on the device.
int main(int argc, char **argv)
{
    // The vendor/product id of the target device. Every Android device exposes its
    // ADB function with these particular ids.
    adbcpp::usb::DeviceId id;
    id.vendor_id = 0x22D9;
    id.product_id = 0x2769;

    // Opening a device that is not attached throws, and the ADB interface may be
    // claimed by a running adb server, so check for it first (blocker 5).
    if (!adbcpp::usb::UsbTransport::is_present(id))
    {
        std::cerr << "warning: no USB device " << std::hex << id.vendor_id << ':' << id.product_id << std::dec
                  << " found; skipping\n";
        return 0;
    }

    try
    {
        adbcpp::usb::UsbTransport transport(id);

        // Load the key adb already authorized, so the device does not prompt. The
        // public key is only needed for the AUTH type 3 fallback (blocker 11).
        const auto key = adbcpp::crypto::Key::load_or_generate();
        const std::string &public_key_string = key.public_key();
        const auto public_key =
            std::span(reinterpret_cast<const std::byte *>(public_key_string.data()), public_key_string.size());

        std::cerr << "key fingerprint: " << key.fingerprint() << '\n';
        std::cerr << "connecting; approve the USB debugging prompt on the device "
                     "if it appears\n";
        adbcpp::Connection connection(transport, public_key,
                                      [&key](std::span<const std::byte> token)
                                      {
                                          return key.sign(token);
                                      });
        if (connection.requested_authorization())
        {
            std::cerr << "the device rejected the signature and requested "
                         "authorization\n";
        }
        std::cout << "connected to a device running protocol 0x" << std::hex << connection.device_version() << std::dec
                  << '\n';

        if (argc > 2 && std::string_view(argv[1]) == "--list")
        {
            for (const auto &entry : adbcpp::list(connection, argv[2]))
            {
                std::cout << (entry.is_directory() ? 'd' : '-') << ' ' << entry.size << ' ' << entry.name << '\n';
            }
            transport.close();
            return 0;
        }

        if (argc > 3 && std::string_view(argv[1]) == "--pull")
        {
            adbcpp::pull(connection, argv[2], argv[3]);
            std::cout << "pulled " << argv[2] << " to " << argv[3] << '\n';
            transport.close();
            return 0;
        }

        if (argc > 3 && std::string_view(argv[1]) == "--push")
        {
            adbcpp::push(connection, argv[2], argv[3]);
            std::cout << "pushed " << argv[2] << " to " << argv[3] << '\n';
            transport.close();
            return 0;
        }

        const std::string command = argc > 1 ? argv[1] : "echo hello";
        const auto result = adbcpp::run(connection, command);
        std::cout << result.output;

        transport.close();
        return result.exit_code;
    }
    catch (const std::exception &error)
    {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
