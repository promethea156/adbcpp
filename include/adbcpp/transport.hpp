#pragma once

#include <cstddef>
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

protected:
    Transport() = default;

    /// A transport is move-only, so a concrete transport can be returned by
    /// value from its factory. The move is protected because only a derived
    /// class's own move constructor calls it.
    Transport(Transport &&) = default;
    Transport &operator=(Transport &&) = default;
};

} // namespace adbcpp
