#pragma once

#include <cstddef>
#include <span>

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
 * Implementations are not required to be thread-safe.
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
   * @return the number of bytes read, or 0 on end of stream.
   */
    virtual std::size_t read(std::span<std::byte> buffer) = 0;

    /**
   * @brief Writes the whole of `data`.
   *
   * @throws std::runtime_error if the data cannot be written.
   */
    virtual void write(std::span<const std::byte> data) = 0;

    /// Closes the transport, releasing any underlying resources.
    virtual void close() = 0;

protected:
    Transport() = default;
};

} // namespace adbcpp
