#include <cstdint>
#include <iomanip>
#include <iostream>

#include "adbcpp/adbcpp.hpp"
#include "adbcpp/testing/mock_transport.hpp"

int main() {
  adbcpp::testing::MockTransport transport;

  adbcpp::protocol::Message device_message;
  device_message.command = adbcpp::protocol::kCnxn;
  device_message.arg0 = 0x01000001u;
  device_message.arg1 = 4096u;
  device_message.magic =
      adbcpp::protocol::Message::compute_magic(device_message.command);
  transport.feed(device_message.encode());

  adbcpp::Session session(transport);
  const auto received = session.receive();

  std::cout << "received command: 0x" << std::hex << std::setw(8)
            << std::setfill('0') << received.command << '\n';
  std::cout << "arg0: 0x" << std::setw(8) << std::setfill('0') << received.arg0
            << '\n';
  std::cout << "arg1: " << std::dec << received.arg1 << '\n';

  return 0;
}
