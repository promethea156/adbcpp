#include "adbcpp/stream.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <utility>

#include "adbcpp/protocol/commands.hpp"

namespace adbcpp
{
namespace
{

protocol::Message make_message(std::uint32_t command, std::uint32_t arg0, std::uint32_t arg1,
                               std::span<const std::byte> payload)
{
    protocol::Message message;
    message.command = command;
    message.arg0 = arg0;
    message.arg1 = arg1;
    message.data_length = protocol::Message::data_length_of(payload);
    message.data_crc32 = protocol::Message::compute_crc32(payload);
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
    // OPEN's destination is null-terminated and the NUL is part of `data_length`,
    // because adbd parses it as a C string. The same is true of the AUTH public
    // key, but not of the CNXN banner (blocker 7 in `04-blockers.md`).
    std::string destination(service_);
    destination.push_back('\0');
    const auto payload = std::span(reinterpret_cast<const std::byte *>(destination.data()), destination.size());
    // `arg1` is the initial delayed-acknowledgement window. A peer that supports
    // the feature closes the stream when it is zero, so a non-zero value is only
    // sent once `delayed_ack` has been negotiated (blocker 12).
    const std::uint32_t send_buffer = connection_->supports_delayed_ack() ? protocol::kInitialDelayedAckBytes : 0;
    connection_->send(make_message(protocol::kOpen, local_id_, send_buffer, payload), payload);

    // The device accepts with OKAY(local-id, remote-id), or refuses with CLSE.
    // Its `arg0` is the device's id for this stream, which is our `remote_id`.
    //
    // Frames carry the recipient's local id in `arg1`, so a stray frame for
    // another stream may arrive first; for example the device may send a second
    // CLOSE for the previous stream while this one opens. Those are skipped.
    auto frame = connection_->receive();
    while (frame.header.arg1 != local_id_)
    {
        frame = connection_->receive();
    }
    if (frame.header.command != protocol::kOkay)
    {
        throw std::runtime_error("adbcpp: failed to open the stream");
    }
    remote_id_ = frame.header.arg0;
}

Stream::~Stream()
{
    // CLOSE(local-id, remote-id, ""). The device does not answer a CLOSE, so this
    // is fire-and-forget and best effort during destruction.
    if (!closed_)
    {
        try
        {
            connection_->send(make_message(protocol::kClse, local_id_, remote_id_, {}));
        }
        catch (...)
        {
            // Closing is best effort during destruction.
        }
    }
}

void Stream::write(std::span<const std::byte> data)
{
    // WRITE(local-id, remote-id, "data"). A single WRTE must fit in one
    // message. The device advertised the largest payload it accepts in its CNXN
    // `arg1`, so a larger write is rejected here rather than framing a message
    // the device cannot read.
    if (data.size() > connection_->max_data())
    {
        throw std::runtime_error("adbcpp: the write is larger than the negotiated maximum payload");
    }
    connection_->send(make_message(protocol::kWrte, local_id_, remote_id_, data), data);
}

bool Stream::receive_more()
{
    while (true)
    {
        auto frame = connection_->receive();
        // `arg1` is the recipient's local id, so a frame for another stream is
        // skipped rather than mistaken for this stream's data.
        if (frame.header.arg1 != local_id_)
        {
            continue;
        }
        switch (frame.header.command)
        {
            case protocol::kWrte:
                // Each WRITE is acknowledged with OKAY so the device may send the next
                // one. Without delayed acknowledgements this handshake is what paces the
                // stream (blocker 12).
                incoming_ = std::move(frame.payload);
                incoming_offset_ = 0;
                connection_->send(make_message(protocol::kOkay, local_id_, remote_id_, {}));
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
                throw std::runtime_error("adbcpp: unexpected message on the stream");
        }
    }
}

void Stream::read(std::span<std::byte> buffer)
{
    std::size_t total = 0;
    while (total < buffer.size())
    {
        if (incoming_offset_ >= incoming_.size())
        {
            if (!receive_more())
            {
                throw std::runtime_error("adbcpp: the device closed the stream");
            }
        }

        const std::size_t available = incoming_.size() - incoming_offset_;
        const std::size_t count = std::min(available, buffer.size() - total);
        std::copy_n(incoming_.begin() + static_cast<std::ptrdiff_t>(incoming_offset_),
                    static_cast<std::ptrdiff_t>(count), buffer.begin() + static_cast<std::ptrdiff_t>(total));
        incoming_offset_ += count;
        total += count;
    }
}

std::vector<std::byte> Stream::read_all()
{
    // A previous read may have left part of a WRTE buffered, so drain it first.
    std::vector<std::byte> output(incoming_.begin() + static_cast<std::ptrdiff_t>(incoming_offset_), incoming_.end());
    incoming_.clear();
    incoming_offset_ = 0;

    while (receive_more())
    {
        output.insert(output.end(), incoming_.begin(), incoming_.end());
        incoming_.clear();
        incoming_offset_ = 0;
    }
    return output;
}

} // namespace adbcpp
