#include "adbcpp/stream.hpp"

#include <stdexcept>

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
    const auto frame = connection_->receive();
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
    // WRITE(local-id, remote-id, "data"). The payload must fit in one message,
    // which is why it must not exceed the negotiated maximum payload size.
    connection_->send(make_message(protocol::kWrte, local_id_, remote_id_, data), data);
}

std::vector<std::byte> Stream::read_all()
{
    std::vector<std::byte> output;
    while (!closed_)
    {
        auto frame = connection_->receive();
        switch (frame.header.command)
        {
            case protocol::kWrte:
                // Each WRITE is acknowledged with OKAY so the device may send the next
                // one. Without delayed acknowledgements this handshake is what paces the
                // stream (blocker 12).
                output.insert(output.end(), frame.payload.begin(), frame.payload.end());
                connection_->send(make_message(protocol::kOkay, local_id_, remote_id_, {}));
                break;
            case protocol::kClse:
                // The device closes its side when the service is done. CLOSE is
                // bidirectional: the same message closes this side in return.
                closed_ = true;
                connection_->send(make_message(protocol::kClse, local_id_, remote_id_, {}));
                break;
            default:
                throw std::runtime_error("adbcpp: unexpected message on the stream");
        }
    }
    return output;
}

} // namespace adbcpp
