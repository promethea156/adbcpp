#include <iostream>

#include "adbcpp/usb/usb_transport.hpp"

int main() {
  adbcpp::usb::DeviceId id;
  id.vendor_id = 0x22D9;
  id.product_id = 0x2769;

  adbcpp::usb::UsbTransport transport(id);
  std::cout << "opened ADB USB transport for "
            << static_cast<int>(id.vendor_id) << ":"
            << static_cast<int>(id.product_id) << '\n';

  transport.close();
  return 0;
}
