#include "adbcpp/stream.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

#include "adbcpp/log.hpp"
#include "adbcpp/protocol/commands.hpp"

namespace adbcpp
{
namespace
{

// Builds a message with no payload. `data_length_of` cannot fail for an empty
// payload, but it returns a `Result`, and a destructor has nowhere to report one.
protocol::Message make_empty_message(std::uint32_t command, std::uint32_t arg0, std::uint32_t arg1)
{
    protocol::Message message;
    message.command = command;
    message.arg0 = arg0;
    message.arg1 = arg1;
    message.magic = protocol::Message::compute_magic(command);
    return message;
}

// Builds a message carrying `payload`.
Result<protocol::Message> make_message(std::uint32_t command, std::uint32_t arg0, std::uint32_t arg1,
                                       std::span<const std::byte> payload)
{
    const auto length = protocol::Message::data_length_of(payload);
    if (!length)
    {
        return tl::unexpected(length.error());
    }

    protocol::Message message;
    message.command = command;
    message.arg0 = arg0;
    message.arg1 = arg1;
    message.data_length = *length;
    message.data_check = protocol::Message::compute_checksum(payload);
    message.magic = protocol::Message::compute_magic(command);
    return message;
}

} // namespace

Stream::Stream(Connection &connection, std::string_view service)
    : connection_(&connection)
    , service_(service)
    ,
    // Local ids are chosen by the sender and must not be zero; adb reserves
    // id 1 for itself, so `Connection` starts handing out ids at 2.
    local_id_(connection.allocate_local_id())
{
    // Nothing is sent here: the OPEN is the only step that can fail, and a
    // constructor cannot report a failure, so `open` sends it.
}

Result<Stream> Stream::open(Connection &connection, std::string_view service)
{
    Stream stream(connection, service);

    if (is_logging(LogLevel::Trace))
    {
        log(LogLevel::Trace, "open " + std::string(service) + " local_id=" + std::to_string(stream.local_id_));
    }

    // OPEN's destination is null-terminated and the NUL is part of `data_length`,
    // because adbd parses it as a C string. The same is true of the AUTH public
    // key, but not of the CNXN banner (blocker 7 in `04-blockers.md`).
    std::string destination(service);
    destination.push_back('\0');
    const auto payload = std::span(reinterpret_cast<const std::byte *>(destination.data()), destination.size());
    // `arg1` is the initial delayed-acknowledgement window. A peer that supports
    // the feature closes the stream when it is zero, so a non-zero value is only
    // sent once `delayed_ack` has been negotiated (blocker 12).
    const std::uint32_t send_buffer = connection.supports_delayed_ack() ? protocol::kInitialDelayedAckBytes : 0;

    const auto message = make_message(protocol::kOpen, stream.local_id_, send_buffer, payload);
    if (!message)
    {
        return tl::unexpected(message.error());
    }
    if (const auto sent = connection.send(*message, payload); !sent)
    {
        return tl::unexpected(sent.error());
    }

    // The device accepts with OKAY(local-id, remote-id), or refuses with CLSE.
    // Its `arg0` is the device's id for this stream, which is our `remote_id`.
    //
    // Frames carry the recipient's local id in `arg1`, so a stray frame for
    // another stream may arrive first; for example the device may send a second
    // CLOSE for the previous stream while this one opens. Those are skipped.
    while (true)
    {
        auto frame = connection.receive();
        if (!frame)
        {
            return tl::unexpected(frame.error());
        }
        if (frame->header.arg1 != stream.local_id_)
        {
            continue;
        }
        if (frame->header.command != protocol::kOkay)
        {
            log(LogLevel::Error, "the device refused to open the stream");
            return tl::unexpected(Error{ErrorCode::Protocol, "failed to open the stream"});
        }
        stream.remote_id_ = frame->header.arg0;
        log(LogLevel::Info, "opened stream local_id=" + std::to_string(stream.local_id_) +
                                " remote_id=" + std::to_string(stream.remote_id_));
        return stream;
    }
}

Stream::~Stream()
{
    // CLOSE(local-id, remote-id, ""). The device does not answer a CLOSE, so this
    // is fire and forget, and a failure is ignored because a destructor has
    // nowhere to report it.
    close_now();
}

Stream::Stream(Stream &&other) noexcept
    : connection_(other.connection_)
    , service_(std::move(other.service_))
    , local_id_(other.local_id_)
    , remote_id_(other.remote_id_)
    , closed_(other.closed_)
    , incoming_(std::move(other.incoming_))
    , incoming_offset_(other.incoming_offset_)
{
    // The moved-from stream no longer owns the stream, so its destructor must not
    // send a CLOSE for it.
    other.closed_ = true;
}

Stream &Stream::operator=(Stream &&other) noexcept
{
    if (this != &other)
    {
        // The stream this one owns is closed before it is replaced.
        close_now();
        connection_ = other.connection_;
        service_ = std::move(other.service_);
        local_id_ = other.local_id_;
        remote_id_ = other.remote_id_;
        closed_ = other.closed_;
        incoming_ = std::move(other.incoming_);
        incoming_offset_ = other.incoming_offset_;
        other.closed_ = true;
    }
    return *this;
}

void Stream::close_now() noexcept
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    (void)connection_->send(make_empty_message(protocol::kClse, local_id_, remote_id_));
}

Status Stream::write(std::span<const std::byte> data)
{
    // WRITE(local-id, remote-id, "data"). A single WRTE must fit in one
    // message. The device advertised the largest payload it accepts in its CNXN
    // `arg1`, so a larger write is rejected here rather than framing a message
    // the device cannot read.
    if (data.size() > connection_->max_data())
    {
        return tl::unexpected(
            Error{ErrorCode::InvalidArgument, "the write is larger than the negotiated maximum payload"});
    }

    const auto message = make_message(protocol::kWrte, local_id_, remote_id_, data);
    if (!message)
    {
        return tl::unexpected(message.error());
    }
    return connection_->send(*message, data);
}

Result<bool> Stream::receive_more()
{
    while (true)
    {
        auto frame = connection_->receive();
        if (!frame)
        {
            return tl::unexpected(frame.error());
        }
        // `arg1` is the recipient's local id, so a frame for another stream is
        // skipped rather than mistaken for this stream's data.
        if (frame->header.arg1 != local_id_)
        {
            continue;
        }
        switch (frame->header.command)
        {
            case protocol::kWrte:
                // Each WRITE is acknowledged with OKAY so the device may send the next
                // one. Without delayed acknowledgements this handshake is what paces the
                // stream (blocker 12).
                incoming_ = std::move(frame->payload);
                incoming_offset_ = 0;
                if (const auto sent = connection_->send(make_empty_message(protocol::kOkay, local_id_, remote_id_));
                    !sent)
                {
                    return tl::unexpected(sent.error());
                }
                return true;
            case protocol::kOkay:
                // The device acknowledges a WRITE we sent, for example a sync request.
                // It carries no data, so the next frame is read instead.
                continue;
            case protocol::kClse:
                // The device closes its side when the service is done. The protocol is
                // explicit that a CLOSE must not be answered with another CLOSE, so this
                // side is simply marked closed.
                closed_ = true;
                return false;
            default:
                return tl::unexpected(Error{ErrorCode::Protocol, "unexpected message on the stream"});
        }
    }
}

Status Stream::read(std::span<std::byte> buffer)
{
    std::size_t total = 0;
    while (total < buffer.size())
    {
        if (incoming_offset_ >= incoming_.size())
        {
            const auto more = receive_more();
            if (!more)
            {
                return tl::unexpected(more.error());
            }
            if (!*more)
            {
                return tl::unexpected(Error{ErrorCode::Protocol, "the device closed the stream"});
            }
        }

        const std::size_t available = incoming_.size() - incoming_offset_;
        const std::size_t count = std::min(available, buffer.size() - total);
        std::copy_n(incoming_.data() + incoming_offset_, count, buffer.data() + total);
        incoming_offset_ += count;
        total += count;
    }
    return {};
}

Result<std::vector<std::byte>> Stream::read_all()
{
    // A previous read may have left part of a WRTE buffered, so drain it first.
    std::vector<std::byte> output(incoming_.data() + incoming_offset_, incoming_.data() + incoming_.size());
    incoming_.clear();
    incoming_offset_ = 0;

    while (true)
    {
        const auto more = receive_more();
        if (!more)
        {
            return tl::unexpected(more.error());
        }
        if (!*more)
        {
            return output;
        }
        output.insert(output.end(), incoming_.begin(), incoming_.end());
        incoming_.clear();
        incoming_offset_ = 0;
    }
}

} // namespace adbcpp
