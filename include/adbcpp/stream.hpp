#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "adbcpp/connection.hpp"
#include "adbcpp/error.hpp"
#include "adbcpp/export.hpp"

namespace adbcpp
{

/**
 * @brief A logical stream to a service on the device.
 *
 * A stream multiplexes a named service (for example `shell,v2,raw:echo hello`)
 * over the one connection. This is the OPEN/READY/WRITE/CLOSE part of
 * `docs/dev/protocol.md`:
 *
 *   https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/docs/dev/protocol.md
 *
 * Opening a stream sends OPEN(local-id, window, "destination") and waits for the
 * device's OKAY(local-id, remote-id). The ids are relative to the sender, so the
 * `remote_id` here is the device's own id for this stream, and every later
 * WRTE/OKAY/CLSE carries both ids.
 *
 * Reads collect WRTE payloads, acknowledging each with OKAY, until the device
 * closes the stream. The window in OPEN's `arg1` is the delayed-acknowledgement
 * "available send bytes"; see `commands.hpp` and blocker 12 in `04-blockers.md`.
 *
 * A `Stream` is not thread-safe. It shares its connection's session and frame
 * routing, and it buffers a partial WRTE across reads, so concurrent calls on one
 * stream must be serialized by the caller.
 */
class ADBCPP_API Stream
{
public:
    /**
     * @brief Opens a stream to `service`.
     *
     * Opening can fail, and a constructor cannot report that, so this is a named
     * factory and the constructor is private. `service` is the whole service
     * string, for example `sync:` or `shell,v2,raw:echo hello`.
     */
    static Result<Stream> open(Connection &connection, std::string_view service);

    ~Stream();

    Stream(const Stream &) = delete;
    Stream &operator=(const Stream &) = delete;

    /// A stream is returned by value, so it moves. The moved-from stream is
    /// marked closed, so its destructor does not send a second `CLOSE`.
    Stream(Stream &&other) noexcept;
    Stream &operator=(Stream &&other) noexcept;

    /// Writes data to the remote stream.
    Status write(std::span<const std::byte> data);

    /**
     * @brief Reads exactly `buffer.size()` bytes from the remote stream.
     *
     * A WRTE payload may be larger or smaller than `buffer`, so the remainder is
     * buffered and handed out by later reads. Each WRTE is acknowledged with
     * OKAY as it is consumed. An error is returned if the device closes the
     * stream first.
     */
    Status read(std::span<std::byte> buffer);

    /// Reads all output until the remote closes the stream.
    Result<std::vector<std::byte>> read_all();

    /// The service this stream was opened for.
    const std::string &service() const noexcept
    {
        return service_;
    }

private:
    // Opening is done by `open`, so the constructor is private.
    Stream(Connection &connection, std::string_view service);

    // Sends CLOSE for this stream if it has not been sent. The destructor and the
    // move assignment both need it.
    void close_now() noexcept;

    // Receives one WRTE into the buffer and acknowledges it with OKAY. Returns
    // false when the device closed its side of the stream instead.
    Result<bool> receive_more();

    Connection *connection_;
    std::string service_;
    std::uint32_t local_id_;
    std::uint32_t remote_id_ = 0;
    bool closed_ = false;
    std::vector<std::byte> incoming_;
    std::size_t incoming_offset_ = 0;
};

} // namespace adbcpp
