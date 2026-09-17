#include "adbcpp/session.hpp"

#include <array>
#include <cstddef>

namespace adbcpp
{
namespace
{

// Fills `buffer` completely. A Transport::read() may return fewer bytes than
// requested (a USB transfer is a fixed-size chunk, a TCP socket may return a
// partial read), so keep reading until the whole header or payload has arrived.
Status read_exact(Transport &transport, std::span<std::byte> buffer)
{
    std::size_t total = 0;
    while (total < buffer.size())
    {
        const auto count = transport.read(buffer.subspan(total));
        if (!count)
        {
            return tl::unexpected(count.error());
        }
        if (*count == 0)
        {
            return tl::unexpected(Error{ErrorCode::Protocol, "unexpected end of stream"});
        }
        total += *count;
    }
    return {};
}

} // namespace

Session::Session(Transport &transport) noexcept
    : transport_(&transport)
{
}

Status Session::send(const protocol::Message &header, std::span<const std::byte> payload)
{
    // The header is always sent on its own, before the payload. The device's
    // transport reads exactly that way, and `data_length` in the header tells it how
    // many payload bytes to expect. Sending an empty payload as its own transfer
    // would be a zero-length packet, which some USB stacks reject, so it is skipped.
    const auto bytes = header.encode();
    if (const auto written = transport_->write(bytes); !written)
    {
        return written;
    }
    if (!payload.empty())
    {
        return transport_->write(payload);
    }
    return {};
}

Result<Frame> Session::receive()
{
    // `data_length` is the authoritative payload size. `data_crc32` is not used to
    // validate the payload: AOSP stopped verifying it, because USB and TCP both
    // provide their own integrity checks.
    std::array<std::byte, protocol::kMessageHeaderSize> bytes{};
    if (const auto read = read_exact(*transport_, bytes); !read)
    {
        return tl::unexpected(read.error());
    }

    Frame frame;
    frame.header = protocol::Message::decode(bytes);
    frame.payload.resize(frame.header.data_length);
    if (!frame.payload.empty())
    {
        if (const auto read = read_exact(*transport_, frame.payload); !read)
        {
            return tl::unexpected(read.error());
        }
    }
    return frame;
}

} // namespace adbcpp
