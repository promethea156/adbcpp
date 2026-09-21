#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

#include "adbcpp/error.hpp"
#include "adbcpp/export.hpp"

namespace adbcpp
{

/**
 * @brief Abstract byte channel to a device.
 *
 * A transport is responsible only for moving bytes. All ADB framing and
 * protocol logic lives above this interface, which keeps the protocol layer
 * independent of the underlying medium (USB, TCP, mock).
 *
 * Implementations are not required to be thread-safe: concurrent use of one
 * transport must be serialized. Different transports are independent, so one
 * transport per thread is how several devices are driven at once.
 *
 * A transport is opened by a concrete factory (for example `UsbTransport::open` or
 * `TcpTransport::open`). The open and the CNXN handshake are retried together by
 * `adbcpp::connect_with_retry` in `connection.hpp`, which is also how a dropped
 * link is reconnected.
 */
class ADBCPP_API Transport
{
public:
    virtual ~Transport() = default;

    Transport(const Transport &) = delete;
    Transport &operator=(const Transport &) = delete;

    /**
     * @brief Reads up to `buffer.size()` bytes into `buffer`.
     *
     * @return the number of bytes read, 0 on end of stream, or the reason the
     * link failed.
     */
    virtual Result<std::size_t> read(std::span<std::byte> buffer) = 0;

    /**
     * @brief Writes the whole of `data`.
     *
     * @return nothing, or the reason the link failed.
     */
    virtual Status write(std::span<const std::byte> data) = 0;

    /// Closes the transport, releasing any underlying resources.
    virtual void close() = 0;

    /**
     * @brief The device's serial, as `adb devices` prints it, or empty when the
     * transport has none.
     *
     * A USB device's serial is its `iSerial` string descriptor and a TCP
     * transport's is its `host:port` endpoint, mirroring adb's `serial_name`. This
     * is the value `usb::DeviceId::serial` selects on, and the default is empty
     * for a transport, such as a mock, that has no serial of its own.
     */
    virtual std::string_view serial() const noexcept
    {
        return {};
    }

    /**
     * @brief Waits until this transport has bytes to read, up to `timeout`.
     *
     * Returns whether it is readable. A timeout is `false` rather than an error:
     * nothing has arrived yet is a normal answer, not a failure. A readable
     * transport's next @ref read returns bytes without blocking, which is what lets
     * one thread drive several transports.
     *
     * The default reports `false`, because a transport that cannot wait has no other
     * answer; a caller's own transport is then never selected by the `wait_readable`
     * helper below. `tcp::TcpTransport`, `usb::UsbTransport`, and the mock
     * override this, and each documents what its wait costs.
     */
    virtual Result<bool> wait_readable(std::chrono::milliseconds timeout);

protected:
    Transport() = default;

    /// A transport is move-only, so a concrete transport can be returned by
    /// value from its factory. The move is protected because only a derived
    /// class's own move constructor calls it.
    Transport(Transport &&) = default;
    Transport &operator=(Transport &&) = default;
};

/**
 * @brief Waits until one of `transports` has bytes to read, or `timeout` passes.
 *
 * Returns the index of a transport that is readable, or nothing when the timeout
 * passed first. The transport at that index has bytes for its next @ref
 * Transport::read, so a caller drives several devices from one thread by waiting
 * here and then reading the one that came back.
 *
 * The wait is a round-robin of each transport's `wait_readable(0)`, repeated
 * with a short sleep until the timeout, because there is no single handle to select
 * over: a TCP transport's handle is a socket, but a USB transport's is not, so
 * `select` cannot cover both. A transport that reports `false` from its
 * `wait_readable` because it cannot wait is therefore never returned.
 *
 * @warning A transport must outlive the wait, because it is borrowed.
 */
ADBCPP_API Result<std::optional<std::size_t>> wait_readable(std::span<Transport *const> transports,
                                                            std::chrono::milliseconds timeout);

} // namespace adbcpp
