#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>

#include "adbcpp/error.hpp"
#include "adbcpp/export.hpp"
#include "adbcpp/transport.hpp"

namespace adbcpp::tcp
{

/**
 * @brief A Transport over a TCP socket, backed by the platform's sockets.
 *
 * This is the transport for a device that is not on the other end of a USB cable,
 * most commonly an emulator, whose ADB listener accepts a plain TCP connection on
 * `localhost:5555`. The protocol above is unchanged: `Connection` and every service
 * work over this transport exactly as they do over `UsbTransport`, because both only
 * move bytes and `Session` owns the framing.
 *
 * The socket uses `TCP_NODELAY`, because the header and its payload are separate
 * writes and Nagle would coalesce them; `adb` disables it for the same reason. A
 * read returns whatever one `recv` delivers, which may be a partial header or
 * payload, and `Session` reads on until it has a whole message.
 *
 * @note There is no third-party dependency, unlike `UsbTransport`, so this type is
 * always available, with or without `ADBCPP_BUILD_USB`.
 */
class ADBCPP_API TcpTransport : public Transport
{
public:
    /// The default timeout for the connection attempt, in milliseconds.
    ///
    /// The connect is made non-blocking and bounded by `select`, so a host that is
    /// unreachable is noticed instead of hanging.
    static constexpr unsigned int kDefaultConnectTimeoutMs = 10000;

    /// The default timeout for a single `recv` or `send`, in milliseconds.
    ///
    /// The timeout measures silence, like the USB transport's, and not how long the
    /// bytes take on the wire. It is short so that a peer that has gone quiet is
    /// noticed quickly, and a timeout is retried rather than failing outright.
    static constexpr unsigned int kDefaultTransferTimeoutMs = 5000;

    /// The default total a read waits before it gives up, in milliseconds.
    ///
    /// A read is retried while it times out with nothing received, because silence
    /// can be transient, or, during the handshake, the user taking their time to
    /// approve the on-device debugging prompt. This bounds that retrying.
    static constexpr unsigned int kDefaultTransferBudgetMs = 120000;

    /**
     * @brief Connects to `endpoint`, a `host:port` pair.
     *
     * `host` is a name or a literal address, and `port` is a service name or a
     * number; both are resolved with `getaddrinfo`, so `localhost:5555`,
     * `127.0.0.1:5555`, and `[::1]:5555` all work. An IPv6 literal is
     * written in brackets.
     *
     * Opening can fail, and a constructor cannot report that, so this is a named
     * factory and the constructor is private.
     */
    static Result<TcpTransport> open(std::string_view endpoint,
                                     unsigned int connect_timeout_ms = kDefaultConnectTimeoutMs,
                                     unsigned int transfer_timeout_ms = kDefaultTransferTimeoutMs,
                                     unsigned int transfer_budget_ms = kDefaultTransferBudgetMs);

    ~TcpTransport() override;

    TcpTransport(const TcpTransport &) = delete;
    TcpTransport &operator=(const TcpTransport &) = delete;

    /// A transport is returned by value, so it moves.
    TcpTransport(TcpTransport &&) noexcept;
    TcpTransport &operator=(TcpTransport &&) noexcept;

    /// Sets the timeout applied to each later `recv` and `send`, in milliseconds.
    void set_transfer_timeout(unsigned int milliseconds) noexcept;

    /// The timeout applied to each `recv` and `send`, in milliseconds.
    unsigned int transfer_timeout() const noexcept;

    /// Sets the total a read waits before giving up, in milliseconds.
    void set_transfer_budget(unsigned int milliseconds) noexcept;

    /// The total a read waits before giving up, in milliseconds.
    unsigned int transfer_budget() const noexcept;

    Result<std::size_t> read(std::span<std::byte> buffer) override;
    Status write(std::span<const std::byte> data) override;
    void close() override;

    /// The `host:port` endpoint, which is what `adb devices` prints for a `tcpip`
    /// device or an emulator.
    std::string_view serial() const noexcept override;

private:
    // Opening is done by `open`, so the constructor is private.
    TcpTransport();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace adbcpp::tcp
