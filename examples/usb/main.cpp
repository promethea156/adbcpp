#include <cstddef>
#include <iostream>
#include <span>
#include <string>

#include "adbcpp/connection.hpp"
#include "adbcpp/crypto/adb_key.hpp"
#include "adbcpp/shell.hpp"
#include "adbcpp/usb/usb_transport.hpp"

int main(int argc, char **argv) {
  adbcpp::usb::DeviceId id;
  id.vendor_id = 0x22D9;
  id.product_id = 0x2769;

  if (!adbcpp::usb::UsbTransport::is_present(id)) {
    std::cerr << "warning: no USB device " << std::hex << id.vendor_id << ':'
              << id.product_id << std::dec << " found; skipping\n";
    return 0;
  }

  try {
    adbcpp::usb::UsbTransport transport(id);

    const auto key = adbcpp::crypto::Key::load_or_generate();
    const std::string &public_key_string = key.public_key();
    const auto public_key = std::span(
        reinterpret_cast<const std::byte *>(public_key_string.data()),
        public_key_string.size());

    std::cerr << "key fingerprint: " << key.fingerprint() << '\n';
    std::cerr << "connecting; approve the USB debugging prompt on the device "
                 "if it appears\n";
    adbcpp::Connection connection(
        transport, public_key,
        [&key](std::span<const std::byte> token) { return key.sign(token); });
    if (connection.requested_authorization()) {
      std::cerr << "the device rejected the signature and requested "
                   "authorization\n";
    }
    std::cout << "connected to a device running protocol 0x" << std::hex
              << connection.device_version() << std::dec << '\n';

    const std::string command = argc > 1 ? argv[1] : "echo hello";
    const auto result = adbcpp::run(connection, command);
    std::cout << result.output;

    transport.close();
    return result.exit_code;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
