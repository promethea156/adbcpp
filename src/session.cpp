#include "adbcpp/session.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <utility>

#include "adbcpp/protocol/commands.hpp"

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

// A framing error desynchronizes the byte stream, and the protocol cannot recover
// from one, so the connection is closed as well as reported.
Error framing_error(Transport &transport, std::string message)
{
    transport.close();
    return Error{ErrorCode::Protocol, std::move(message)};
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
    std::array<std::byte, protocol::kMessageHeaderSize> bytes{};
    if (const auto read = read_exact(*transport_, bytes); !read)
    {
        return tl::unexpected(read.error());
    }

    Frame frame;
    frame.header = protocol::Message::decode(bytes);

    // `magic` is the inverse of `command`, so a mismatch means the byte stream is
    // desynchronized. The check is the header's own integrity check, and AOSP's
    // `check_header` rejects the same condition.
    if (frame.header.magic != protocol::Message::compute_magic(frame.header.command))
    {
        return tl::unexpected(framing_error(*transport_, "the header magic does not match its command"));
    }

    // `data_length` is authoritative for how much payload to read, so a value past
    // the protocol's maximum means a corrupt header; without the bound a garbage
    // length would be allocated and awaited. AOSP's `check_header` bounds it by the
    // transport's maximum payload.
    if (frame.header.data_length > protocol::kMaxData)
    {
        return tl::unexpected(framing_error(*transport_, "the header payload length is past the maximum"));
    }

    frame.payload.resize(frame.header.data_length);
    if (!frame.payload.empty())
    {
        if (const auto read = read_exact(*transport_, frame.payload); !read)
        {
            return tl::unexpected(read.error());
        }

        // The CRC is advisory: protocol 0x01000001 and later send it as zero and do
        // not compute it (AOSP's `A_VERSION_SKIP_CHECKSUM`), so it is verified only
        // when a sender set it.
        if (frame.header.data_crc32 != 0 && frame.header.data_crc32 != protocol::Message::compute_crc32(frame.payload))
        {
            return tl::unexpected(framing_error(*transport_, "the header payload CRC does not match the payload"));
        }
    }
    return frame;
}

} // namespace adbcpp
