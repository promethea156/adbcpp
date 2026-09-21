#include "adbcpp/log.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "adbcpp/connection.hpp"
#include "adbcpp/protocol/commands.hpp"
#include "adbcpp/protocol/message.hpp"
#include "adbcpp/testing/mock_transport.hpp"

// Logging is opt-in and process-wide: a sink is installed with `set_logger`, and
// the library writes protocol events to it. These tests pin down that the frames
// of the handshake are written, and that the key material never is.
using adbcpp::protocol::Message;

namespace
{

// Collects the messages a test's sink receives, and removes the sink when it goes
// out of scope, so one test's logger does not leak into the next.
class LogCapture
{
public:
    explicit LogCapture(adbcpp::LogLevel level = adbcpp::LogLevel::Trace)
    {
        adbcpp::set_logger(
            [this](adbcpp::LogLevel message_level, std::string_view message)
            {
                messages.emplace_back(message_level, std::string(message));
            },
            level);
    }

    ~LogCapture()
    {
        adbcpp::clear_logger();
    }

    LogCapture(const LogCapture &) = delete;
    LogCapture &operator=(const LogCapture &) = delete;

    /// Whether any message contains `needle`.
    bool contains(std::string_view needle) const
    {
        for (const auto &[level, message] : messages)
        {
            if (message.find(needle) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    /// Whether any message contains the byte `needle`.
    bool contains_byte(std::byte needle) const
    {
        for (const auto &[level, message] : messages)
        {
            if (message.find(static_cast<char>(needle)) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    std::vector<std::pair<adbcpp::LogLevel, std::string>> messages;
};

adbcpp::protocol::Message make_message(std::uint32_t command, std::uint32_t arg0, std::uint32_t arg1,
                                       std::span<const std::byte> payload = {})
{
    adbcpp::protocol::Message message;
    message.command = command;
    message.arg0 = arg0;
    message.arg1 = arg1;
    message.data_length = payload.size();
    message.data_check = Message::compute_checksum(payload);
    message.magic = Message::compute_magic(command);
    return message;
}

} // namespace

TEST_CASE("logging is off until a sink is installed", "[log]")
{
    adbcpp::clear_logger();

    REQUIRE_FALSE(adbcpp::is_logging(adbcpp::LogLevel::Error));

    // With no sink, `log` does nothing rather than failing.
    adbcpp::log(adbcpp::LogLevel::Error, "nothing");
}

TEST_CASE("a sink receives the levels at or below its level", "[log]")
{
    LogCapture capture(adbcpp::LogLevel::Warning);

    REQUIRE(adbcpp::is_logging(adbcpp::LogLevel::Error));
    REQUIRE(adbcpp::is_logging(adbcpp::LogLevel::Warning));
    REQUIRE_FALSE(adbcpp::is_logging(adbcpp::LogLevel::Info));

    adbcpp::log(adbcpp::LogLevel::Error, "an error");
    adbcpp::log(adbcpp::LogLevel::Warning, "a warning");
    adbcpp::log(adbcpp::LogLevel::Info, "an info");

    REQUIRE(capture.contains("an error"));
    REQUIRE(capture.contains("a warning"));
    REQUIRE_FALSE(capture.contains("an info"));
}

TEST_CASE("a sink sees the CNXN and AUTH frames of the handshake", "[log]")
{
    LogCapture capture;
    adbcpp::testing::MockTransport transport;

    const std::array<std::byte, 20> token{};
    const auto auth = make_message(adbcpp::protocol::kAuth, adbcpp::protocol::kAuthToken, 0, token);
    transport.feed(auth.encode());
    transport.feed(token);

    const auto device = make_message(adbcpp::protocol::kCnxn, 0x01000001u, 4096u);
    transport.feed(device.encode());

    const std::array<std::byte, 4> public_key{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
    const auto connection = adbcpp::Connection::connect(transport, public_key);
    REQUIRE(connection.has_value());

    REQUIRE(capture.contains("CNXN"));
    REQUIRE(capture.contains("AUTH"));
    REQUIRE(capture.contains("send"));
    REQUIRE(capture.contains("receive"));
}

TEST_CASE("a sink never sees the key material", "[log]")
{
    LogCapture capture;
    adbcpp::testing::MockTransport transport;

    // The token, the signature, and the public key are each a distinctive byte,
    // so the test can tell whether any of them reached the sink.
    const std::vector<std::byte> token(20, std::byte{0xAA});
    const auto auth = make_message(adbcpp::protocol::kAuth, adbcpp::protocol::kAuthToken, 0, token);
    transport.feed(auth.encode());
    transport.feed(token);
    transport.feed(auth.encode());
    transport.feed(token);

    const auto device = make_message(adbcpp::protocol::kCnxn, 0x01000001u, 4096u);
    transport.feed(device.encode());

    const std::vector<std::byte> public_key(4, std::byte{0xCC});
    const std::vector<std::byte> signature(256, std::byte{0xBB});
    const adbcpp::Connection::Signer signer = [signature](std::span<const std::byte>)
    {
        return adbcpp::Result<std::vector<std::byte>>(signature);
    };

    const auto connection = adbcpp::Connection::connect(transport, public_key, signer);
    REQUIRE(connection.has_value());

    REQUIRE(capture.contains("AUTH"));
    REQUIRE_FALSE(capture.contains_byte(std::byte{0xAA}));
    REQUIRE_FALSE(capture.contains_byte(std::byte{0xBB}));
    REQUIRE_FALSE(capture.contains_byte(std::byte{0xCC}));
}
