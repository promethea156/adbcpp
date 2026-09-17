#pragma once

#include <cstddef>
#include <span>

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

protected:
    Transport() = default;

    /// A transport is move-only, so a concrete transport can be returned by
    /// value from its factory. The move is protected because only a derived
    /// class's own move constructor calls it.
    Transport(Transport &&) = default;
    Transport &operator=(Transport &&) = default;
};

} // namespace adbcpp
