#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>

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

} // namespace

int main()
{
    adbcpp::usb::DeviceId id;
    id.vendor_id = 0x22D9;
    id.product_id = 0x2769;

    if (!adbcpp::usb::UsbTransport::is_present(id))
    {
        std::cerr << "no USB device " << std::hex << id.vendor_id << ':' << id.product_id << std::dec
                  << " found; skipping\n";
        return kSkip;
    }

    try
    {
        adbcpp::usb::UsbTransport transport(id);

        const auto key = adbcpp::crypto::Key::load_or_generate();
        const std::string &public_key_string = key.public_key();
        const auto public_key =
            std::span(reinterpret_cast<const std::byte *>(public_key_string.data()), public_key_string.size());

        std::cerr << "connecting; approve the USB debugging prompt on the device "
                     "if it appears\n";
        adbcpp::Connection connection(transport, public_key,
                                      [&key](std::span<const std::byte> token)
                                      {
                                          return key.sign(token);
                                      });

        const auto result = adbcpp::run(connection, "echo hello");
        if (result.output != "hello\n")
        {
            std::cerr << "unexpected output: " << result.output << '\n';
            return 1;
        }
        if (result.exit_code != 0)
        {
            std::cerr << "unexpected exit code: " << static_cast<int>(result.exit_code) << '\n';
            return 1;
        }

        // `/` is always present and always holds at least `sdcard`.
        const auto entries = adbcpp::list(connection, "/");
        transport.close();
        if (entries.empty())
        {
            std::cerr << "the listing of / is empty\n";
            return 1;
        }
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
