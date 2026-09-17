#include <cstdint>
#include <iomanip>
#include <iostream>

#include "adbcpp/adbcpp.hpp"
#include "adbcpp/testing/mock_transport.hpp"

// A minimal protocol round-trip without a device: queue a device CNXN header on
// the mock transport and read it back through the `Session` framing layer. This is
// the Slice 0 walking skeleton, kept as a small example.
int main()
{
    adbcpp::testing::MockTransport transport;

    // Build CNXN(version, maxdata, ""). The magic is always the inverse of the
    // command; the device would set the same fields on a real connection.
    adbcpp::protocol::Message device_message;
    device_message.command = adbcpp::protocol::kCnxn;
    device_message.arg0 = 0x01000001u;
    device_message.arg1 = 4096u;
    device_message.magic = adbcpp::protocol::Message::compute_magic(device_message.command);
    transport.feed(device_message.encode());

    // `Session` reads the 24-byte header queued above. There is no payload
    // because CNXN's data_length defaults to zero.
    adbcpp::Session session(transport);
    const auto frame = session.receive();

    std::cout << "received command: 0x" << std::hex << std::setw(8) << std::setfill('0') << frame.header.command
              << '\n';
    std::cout << "arg0: 0x" << std::setw(8) << std::setfill('0') << frame.header.arg0 << '\n';
    std::cout << "arg1: " << std::dec << frame.header.arg1 << '\n';

    return 0;
}
