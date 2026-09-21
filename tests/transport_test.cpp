#include "adbcpp/transport.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <span>
#include <vector>

#include "adbcpp/testing/mock_transport.hpp"

// `adbcpp::wait_readable` waits on several transports at once, so one thread can
// drive several devices. These tests pin down that it reports the readable one, that
// it times out rather than blocking when none is, and that a transport which
// cannot wait is never reported.

namespace
{

// A transport that implements only the three required operations, standing in for a
// caller's own transport that does not override `wait_readable`.
class MinimalTransport : public adbcpp::Transport
{
public:
    adbcpp::Result<std::size_t> read(std::span<std::byte>) override
    {
        return 0;
    }

    adbcpp::Status write(std::span<const std::byte>) override
    {
        return {};
    }

    void close() override
    {
    }
};

constexpr std::array<std::byte, 3> kBytes{std::byte{'h'}, std::byte{'i'}, std::byte{'!'}};

} // namespace

TEST_CASE("a transport that cannot wait reports not readable", "[transport]")
{
    MinimalTransport transport;

    const auto readable = transport.wait_readable(std::chrono::milliseconds(5));
    REQUIRE(readable.has_value());
    REQUIRE_FALSE(*readable);
}

TEST_CASE("a mock transport is readable once bytes are queued", "[transport]")
{
    adbcpp::testing::MockTransport transport;

    const auto before = transport.wait_readable(std::chrono::milliseconds(5));
    REQUIRE(before.has_value());
    REQUIRE_FALSE(*before);

    transport.feed(kBytes);

    const auto after = transport.wait_readable(std::chrono::milliseconds(5));
    REQUIRE(after.has_value());
    REQUIRE(*after);
}

TEST_CASE("wait_readable reports nothing when no transport is readable", "[transport]")
{
    adbcpp::testing::MockTransport first;
    adbcpp::testing::MockTransport second;

    const std::vector<adbcpp::Transport *> transports{&first, &second};
    const auto readable = adbcpp::wait_readable(transports, std::chrono::milliseconds(5));

    REQUIRE(readable.has_value());
    REQUIRE_FALSE(readable->has_value());
}

TEST_CASE("wait_readable reports the readable transport", "[transport]")
{
    adbcpp::testing::MockTransport first;
    adbcpp::testing::MockTransport second;

    // Only the second transport has bytes, so the wait must report index 1 and not
    // the first transport.
    second.feed(kBytes);

    const std::vector<adbcpp::Transport *> transports{&first, &second};
    const auto readable = adbcpp::wait_readable(transports, std::chrono::milliseconds(5));

    REQUIRE(readable.has_value());
    REQUIRE(readable->has_value());
    REQUIRE(**readable == 1);
}

TEST_CASE("wait_readable never reports a transport that cannot wait", "[transport]")
{
    MinimalTransport minimal;
    adbcpp::testing::MockTransport mock;
    mock.feed(kBytes);

    const std::vector<adbcpp::Transport *> transports{&minimal, &mock};
    const auto readable = adbcpp::wait_readable(transports, std::chrono::milliseconds(5));

    REQUIRE(readable.has_value());
    REQUIRE(readable->has_value());
    REQUIRE(**readable == 1);
}
