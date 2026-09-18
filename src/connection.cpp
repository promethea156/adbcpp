#include "adbcpp/connection.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "adbcpp/protocol/commands.hpp"

namespace adbcpp
{
namespace
{

// Builds an AUTH message. Its shape is AUTH(type, 0, "data") in the notation of
// `docs/dev/protocol.md`: `type` is one of the `kAuth*` constants and `arg1` is
// always zero. The payload is a token, a signature, or a public key depending on
// `type`.
Result<protocol::Message> make_auth(std::uint32_t type, std::span<const std::byte> payload)
{
    const auto length = protocol::Message::data_length_of(payload);
    if (!length)
    {
        return tl::unexpected(length.error());
    }

    protocol::Message auth;
    auth.command = protocol::kAuth;
    auth.arg0 = type;
    auth.data_length = *length;
    auth.data_check = protocol::Message::compute_checksum(payload);
    auth.magic = protocol::Message::compute_magic(auth.command);
    return auth;
}

// Parses the `features=<a,b,c>` list from a CNXN banner. The banner may hold
// other `key=value` properties separated by `;`, so the list ends at the next `;`
// or at the end of the banner.
std::vector<std::string> parse_features(std::string_view banner)
{
    constexpr std::string_view kPrefix = "features=";
    const std::size_t position = banner.find(kPrefix);
    if (position == std::string_view::npos)
    {
        return {};
    }

    std::string_view list = banner.substr(position + kPrefix.size());
    const std::size_t end = list.find(';');
    if (end != std::string_view::npos)
    {
        list = list.substr(0, end);
    }

    std::vector<std::string> features;
    std::size_t start = 0;
    while (start <= list.size())
    {
        const std::size_t comma = list.find(',', start);
        const std::size_t stop = comma == std::string_view::npos ? list.size() : comma;
        if (stop > start)
        {
            features.emplace_back(list.substr(start, stop - start));
        }
        if (comma == std::string_view::npos)
        {
            break;
        }
        start = comma + 1;
    }
    return features;
}

} // namespace

Connection::Connection(Transport &transport)
    : session_(transport)
{
}

Result<Connection> Connection::connect(Transport &transport, std::span<const std::byte> public_key, Signer signer,
                                       bool advertise_delayed_ack)
{
    Connection connection(transport);

    // CNXN(version, maxdata, "system-identity-string"). `arg1` is the largest
    // payload the peer may send us; it must be at least large enough for a 256-byte
    // AUTH signature. Unlike the OPEN/AUTH payloads, the banner is not
    // null-terminated: adb passes its length, and a capture confirms a 286-byte
    // payload with no trailing NUL (blocker 7 in `04-blockers.md`).
    std::string identity(kSystemIdentity);
    if (advertise_delayed_ack)
    {
        identity += ',';
        identity += kDelayedAckFeature;
    }
    const auto identity_bytes = std::span(reinterpret_cast<const std::byte *>(identity.data()), identity.size());

    const auto length = protocol::Message::data_length_of(identity_bytes);
    if (!length)
    {
        return tl::unexpected(length.error());
    }

    protocol::Message connect_message;
    connect_message.command = protocol::kCnxn;
    connect_message.arg0 = protocol::kVersion;
    connect_message.arg1 = protocol::kMaxData;
    connect_message.data_length = *length;
    connect_message.data_check = protocol::Message::compute_checksum(identity_bytes);
    connect_message.magic = protocol::Message::compute_magic(connect_message.command);
    if (const auto sent = connection.send(connect_message, identity_bytes); !sent)
    {
        return tl::unexpected(sent.error());
    }

    // The device answers CNXN directly when it trusts us, or AUTH when it wants
    // authentication. It may also send AUTH again after our signature, which means
    // it did not recognise it.
    auto frame = connection.receive();
    if (!frame)
    {
        return tl::unexpected(frame.error());
    }

    if (frame->header.command == protocol::kAuth)
    {
        if (signer)
        {
            // AUTH type 2: the 256-byte PKCS#1 v1.5 SHA-1 signature of the token.
            // `Key::sign` signs the token as-is; re-hashing it here was blocker 16.
            const auto signature = signer(frame->payload);
            if (!signature)
            {
                return tl::unexpected(signature.error());
            }

            const auto auth = make_auth(protocol::kAuthSignature, *signature);
            if (!auth)
            {
                return tl::unexpected(auth.error());
            }
            if (const auto sent = connection.send(*auth, *signature); !sent)
            {
                return tl::unexpected(sent.error());
            }

            frame = connection.receive();
            if (!frame)
            {
                return tl::unexpected(frame.error());
            }
        }

        // The device did not recognize the signature; offer the public key so it can
        // ask the user to authorize it, exactly like adb.
        if (frame->header.command == protocol::kAuth)
        {
            connection.requested_authorization_ = true;
            if (public_key.empty())
            {
                return tl::unexpected(
                    Error{ErrorCode::Crypto,
                          "the device rejected the signature and no public key is available to request authorization"});
            }
            // AUTH type 3: the public key string, null-terminated and including the
            // NUL in `data_length`, because adbd parses it as a C string.
            std::vector<std::byte> key(public_key.begin(), public_key.end());
            key.push_back(std::byte{0});

            const auto auth = make_auth(protocol::kAuthPublicKey, key);
            if (!auth)
            {
                return tl::unexpected(auth.error());
            }
            if (const auto sent = connection.send(*auth, key); !sent)
            {
                return tl::unexpected(sent.error());
            }

            frame = connection.receive();
            if (!frame)
            {
                return tl::unexpected(frame.error());
            }
        }
    }

    if (frame->header.command != protocol::kCnxn)
    {
        return tl::unexpected(Error{ErrorCode::Protocol, "unexpected response to the CNXN message"});
    }

    connection.device_version_ = frame->header.arg0;
    connection.max_data_ = frame->header.arg1;

    // The device's banner reports its own features. They select the protocol
    // variants, for example `ls_v2` for the v2 LIST/DENT form.
    const std::string banner(reinterpret_cast<const char *>(frame->payload.data()), frame->payload.size());
    connection.features_ = parse_features(banner);

    // `delayed_ack` is only enabled if both sides advertised it, so a device
    // that does not support it keeps the OPEN window at zero (blocker 12).
    connection.delayed_ack_ = advertise_delayed_ack && connection.supports_feature(kDelayedAckFeature);

    return connection;
}

bool Connection::supports_feature(std::string_view feature) const noexcept
{
    return std::find(features_.begin(), features_.end(), feature) != features_.end();
}

Status Connection::send(const protocol::Message &header, std::span<const std::byte> payload)
{
    return session_.send(header, payload);
}

Result<Frame> Connection::receive()
{
    return session_.receive();
}

} // namespace adbcpp
