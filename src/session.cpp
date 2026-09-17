#include "adbcpp/session.hpp"

#include <array>
#include <cstddef>
#include <stdexcept>

namespace adbcpp
{
namespace
{

// Fills `buffer` completely. A Transport::read() may return fewer bytes than
// requested (a USB transfer is a fixed-size chunk, a TCP socket may return a
// partial read), so keep reading until the whole header or payload has arrived.
void read_exact(Transport &transport, std::span<std::byte> buffer)
{
    std::size_t total = 0;
    while (total < buffer.size())
    {
        const std::size_t count = transport.read(buffer.subspan(total));
        if (count == 0)
        {
            throw std::runtime_error("adbcpp: unexpected end of stream");
        }
        total += count;
    }
}

} // namespace

Session::Session(Transport &transport) noexcept
    : transport_(&transport)
{
}

void Session::send(const protocol::Message &header,
                   std::span<const std::byte> payload)
{
    // The header is always sent on its own, before the payload. The device's
    // transport reads exactly that way, and `data_length` in the header tells it how
    // many payload bytes to expect. Sending an empty payload as its own transfer
    // would be a zero-length packet, which some USB stacks reject, so it is skipped.
    const auto bytes = header.encode();
    transport_->write(bytes);
    if (!payload.empty())
    {
        transport_->write(payload);
    }
}

Frame Session::receive()
{
    // `data_length` is the authoritative payload size. `data_crc32` is not used to
    // validate the payload: AOSP stopped verifying it, because USB and TCP both
    // provide their own integrity checks.
    std::array<std::byte, protocol::kMessageHeaderSize> bytes{};
    read_exact(*transport_, bytes);

    Frame frame;
    frame.header = protocol::Message::decode(bytes);
    frame.payload.resize(frame.header.data_length);
    if (!frame.payload.empty())
    {
        read_exact(*transport_, frame.payload);
    }
    return frame;
}

} // namespace adbcpp
