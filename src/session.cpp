#include "adbcpp/session.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "adbcpp/log.hpp"
#include "adbcpp/protocol/commands.hpp"

namespace adbcpp
{
namespace
{

// The four-character name of `command`, for a log message. An unknown command is
// named "????", because it is not one the library sends and the numeric value is
// logged alongside it.
std::string_view command_name(std::uint32_t command) noexcept
{
    switch (command)
    {
        case protocol::kCnxn:
            return "CNXN";
        case protocol::kAuth:
            return "AUTH";
        case protocol::kOpen:
            return "OPEN";
        case protocol::kOkay:
            return "OKAY";
        case protocol::kClse:
            return "CLSE";
        case protocol::kWrte:
            return "WRTE";
        case protocol::kSync:
            return "SYNC";
        default:
            return "????";
    }
}

// Describes a header for a log message. The payload is never included: it can be a
// key, a signature, or a file's bytes, and the command, its arguments, and its
// length are enough to follow the protocol.
std::string describe(const protocol::Message &header)
{
    return std::string(command_name(header.command)) + " arg0=" + std::to_string(header.arg0) +
           " arg1=" + std::to_string(header.arg1) + " data_length=" + std::to_string(header.data_length);
}

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

// A framing error desynchronizes the byte stream, and the protocol cannot recover
// from one, so the session is closed as well as reported.
Error Session::framing_error(std::string message)
{
    log(LogLevel::Error, "framing error: " + message);
    close();
    return Error{ErrorCode::Protocol, std::move(message)};
}

Session::Session(Transport &transport) noexcept
    : transport_(&transport)
{
}

Session::Session(Session &&other) noexcept
    : transport_(other.transport_)
    , closed_(other.closed_)
{
    // The moved-from session no longer owns the transport, so its close() must not
    // close the transport the moved-to session uses.
    other.closed_ = true;
}

Session &Session::operator=(Session &&other) noexcept
{
    if (this != &other)
    {
        // The transport is borrowed, so the old one is not closed here.
        transport_ = other.transport_;
        closed_ = other.closed_;
        other.closed_ = true;
    }
    return *this;
}

void Session::close() noexcept
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    transport_->close();
}

Status Session::send(const protocol::Message &header, std::span<const std::byte> payload)
{
    if (closed_)
    {
        return tl::unexpected(Error{ErrorCode::Transport, "the session is closed"});
    }

    if (is_logging(LogLevel::Debug))
    {
        log(LogLevel::Debug, "send " + describe(header));
    }

    // The header is always sent on its own, before the payload. The device's
    // transport reads exactly that way, and `data_length` in the header tells it how
    // many payload bytes to expect. Sending an empty payload as its own transfer
    // would be a zero-length packet, which some USB stacks reject, so it is skipped.
    const auto bytes = header.encode();
    if (const auto written = transport_->write(bytes); !written)
    {
        log(LogLevel::Error, "send failed: " + written.error().message);
        return written;
    }
    if (!payload.empty())
    {
        if (const auto written = transport_->write(payload); !written)
        {
            log(LogLevel::Error, "send failed: " + written.error().message);
            return written;
        }
    }
    return {};
}

Result<Frame> Session::receive()
{
    if (closed_)
    {
        return tl::unexpected(Error{ErrorCode::Transport, "the session is closed"});
    }

    std::array<std::byte, protocol::kMessageHeaderSize> bytes{};
    if (const auto read = read_exact(*transport_, bytes); !read)
    {
        log(LogLevel::Error, "receive failed: " + read.error().message);
        return tl::unexpected(read.error());
    }

    Frame frame;
    frame.header = protocol::Message::decode(bytes);
    if (is_logging(LogLevel::Debug))
    {
        log(LogLevel::Debug, "receive " + describe(frame.header));
    }

    // `magic` is the inverse of `command`, so a mismatch means the byte stream is
    // desynchronized. The check is the header's own integrity check, and AOSP's
    // `check_header` rejects the same condition.
    if (frame.header.magic != protocol::Message::compute_magic(frame.header.command))
    {
        return tl::unexpected(framing_error("the header magic does not match its command"));
    }

    // `data_length` is authoritative for how much payload to read, so a value past
    // the protocol's maximum means a corrupt header; without the bound a garbage
    // length would be allocated and awaited. AOSP's `check_header` bounds it by the
    // transport's maximum payload.
    if (frame.header.data_length > protocol::kMaxData)
    {
        return tl::unexpected(framing_error("the header payload length is past the maximum"));
    }

    frame.payload.resize(frame.header.data_length);
    if (!frame.payload.empty())
    {
        if (const auto read = read_exact(*transport_, frame.payload); !read)
        {
            log(LogLevel::Error, "receive failed: " + read.error().message);
            return tl::unexpected(read.error());
        }

        // The checksum is advisory: a device on protocol 0x01000001 and later
        // sends zero, while adb computes it for the handshake's CNXN and AUTH, so
        // it is verified only when a sender set it.
        if (frame.header.data_check != 0 &&
            frame.header.data_check != protocol::Message::compute_checksum(frame.payload))
        {
            return tl::unexpected(framing_error("the header payload checksum does not match the payload"));
        }
    }
    return frame;
}

} // namespace adbcpp
