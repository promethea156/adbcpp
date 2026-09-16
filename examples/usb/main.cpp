#include <cstddef>
#include <iostream>
#include <span>
#include <string>

#include "adbcpp/connection.hpp"
#include "adbcpp/crypto/adb_key.hpp"
#include "adbcpp/shell.hpp"
#include "adbcpp/usb/usb_transport.hpp"

int main() {
  try {
    adbcpp::usb::DeviceId id;
    id.vendor_id = 0x22D9;
    id.product_id = 0x2769;

    adbcpp::usb::UsbTransport transport(id);

    const std::string key = adbcpp::crypto::load_or_generate_public_key();
    const auto public_key = std::span(
        reinterpret_cast<const std::byte *>(key.data()), key.size());

    adbcpp::Connection connection(transport, public_key);
    std::cout << "connected to a device running protocol 0x" << std::hex
              << connection.device_version() << std::dec << '\n';

    const auto result = adbcpp::run(connection, "echo hello");
    std::cout << result.output;

    transport.close();
    return result.exit_code;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
