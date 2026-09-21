#include "adbcpp/transport.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <thread>

namespace adbcpp
{
namespace
{

// How long the round-robin waits between passes. It is short, so a transport
// that becomes readable is noticed quickly, and long enough not to spin.
constexpr std::chrono::milliseconds kPollInterval{1};

} // namespace

// A transport that cannot wait has no other answer than "not readable", so the
// default reports that rather than failing. See the class for why.
Result<bool> Transport::wait_readable(std::chrono::milliseconds)
{
    return false;
}

Result<std::optional<std::size_t>> wait_readable(std::span<Transport *const> transports,
                                                 std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true)
    {
        for (std::size_t index = 0; index < transports.size(); ++index)
        {
            // A zero timeout asks without waiting, which is what the round-robin
            // needs: each transport is asked in turn rather than one blocking.
            const auto readable = transports[index]->wait_readable(std::chrono::milliseconds::zero());
            if (!readable)
            {
                return tl::unexpected(readable.error());
            }
            if (*readable)
            {
                return index;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return std::nullopt;
        }
        std::this_thread::sleep_for(kPollInterval);
    }
}

} // namespace adbcpp
