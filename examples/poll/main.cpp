#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "adbcpp/adbcpp.hpp"
#include "adbcpp/tcp/tcp_transport.hpp"

#if defined(_WIN32)
#    include <winsock2.h>
#    include <ws2tcpip.h>
#else
#    include <arpa/inet.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <unistd.h>
#endif

// Drives two devices from one thread. `adbcpp::wait_readable` waits on both
// transports at once, so one thread services whichever device has bytes, rather
// than one thread per device. The per-thread model is in `examples/multi`.
//
// The two "devices" are loopback listeners that stand in for an emulator's ADB
// listener, so the example runs with no device attached and in CI. The same code
// works unchanged against two `TcpTransport`s to two devices, or two USB devices;
// only the transport lines change.
namespace
{

#if defined(_WIN32)
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

// The device's id for the first stream. The ids are relative to the sender, so the
// client's own id for the stream is chosen by the library.
constexpr std::uint32_t kDeviceId = 7;

void close_socket(socket_t socket) noexcept
{
#if defined(_WIN32)
    ::closesocket(socket);
#else
    ::close(socket);
#endif
}

#if defined(_WIN32)
// The listener needs Winsock before the transport initializes it.
struct SocketRuntime
{
    SocketRuntime()
    {
        WSADATA data{};
        (void)WSAStartup(MAKEWORD(2, 2), &data);
    }
};
#endif

// A loopback listener on an ephemeral port, accepted from a background thread.
class Loopback
{
public:
    Loopback()
    {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ == kInvalidSocket)
        {
            std::exit(1);
        }
        const int enabled = 1;
        ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&enabled), sizeof(enabled));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(listener_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
            ::listen(listener_, 1) != 0)
        {
            std::exit(1);
        }

        socklen_t size = sizeof(address);
        (void)::getsockname(listener_, reinterpret_cast<sockaddr *>(&address), &size);
        port_ = ntohs(address.sin_port);
    }

    ~Loopback()
    {
        if (listener_ != kInvalidSocket)
        {
            close_socket(listener_);
        }
    }

    Loopback(const Loopback &) = delete;
    Loopback &operator=(const Loopback &) = delete;

    std::string endpoint() const
    {
        return "127.0.0.1:" + std::to_string(port_);
    }

    socket_t accept() const
    {
        return ::accept(listener_, nullptr, nullptr);
    }

private:
    socket_t listener_ = kInvalidSocket;
    std::uint16_t port_ = 0;
};

struct Frame
{
    adbcpp::protocol::Message header;
    std::vector<std::byte> payload;
};

// Reads exactly one header and its payload, because a socket read may return a
// partial header or payload.
void read_exact(socket_t socket, std::span<std::byte> buffer)
{
    std::size_t total = 0;
    while (total < buffer.size())
    {
        const int received =
            ::recv(socket, reinterpret_cast<char *>(buffer.data()) + total, static_cast<int>(buffer.size() - total), 0);
        if (received <= 0)
        {
            std::exit(1);
        }
        total += static_cast<std::size_t>(received);
    }
}

Frame read_frame(socket_t socket)
{
    std::array<std::byte, adbcpp::protocol::kMessageHeaderSize> header{};
    read_exact(socket, header);

    Frame frame;
    frame.header = adbcpp::protocol::Message::decode(header);
    frame.payload.resize(frame.header.data_length);
    if (!frame.payload.empty())
    {
        read_exact(socket, frame.payload);
    }
    return frame;
}

void write_frame(socket_t socket, std::uint32_t command, std::uint32_t arg0, std::uint32_t arg1,
                 std::span<const std::byte> payload = {})
{
    adbcpp::protocol::Message header;
    header.command = command;
    header.arg0 = arg0;
    header.arg1 = arg1;
    header.data_length = static_cast<std::uint32_t>(payload.size());
    header.data_check = adbcpp::protocol::Message::compute_checksum(payload);
    header.magic = adbcpp::protocol::Message::compute_magic(command);

    const auto bytes = header.encode();
    (void)::send(socket, reinterpret_cast<const char *>(bytes.data()), static_cast<int>(bytes.size()), 0);
    if (!payload.empty())
    {
        (void)::send(socket, reinterpret_cast<const char *>(payload.data()), static_cast<int>(payload.size()), 0);
    }
}

std::span<const std::byte> bytes_of(const std::string &text)
{
    return std::span(reinterpret_cast<const std::byte *>(text.data()), text.size());
}

// Stands in for adbd: answers the CNXN, accepts the OPEN, and writes the v1
// shell's raw output followed by its CLOSE. The v1 service is used so the output
// is not framed, which keeps the example's own bookkeeping small.
void serve_device(socket_t socket, const std::string &output)
{
    const auto cnxn = read_frame(socket);
    if (cnxn.header.command != adbcpp::protocol::kCnxn)
    {
        std::exit(1);
    }

    const std::string banner = "device::features=";
    write_frame(socket, adbcpp::protocol::kCnxn, 0x01000001u, 4096u, bytes_of(banner));

    const auto open = read_frame(socket);
    const std::uint32_t remote = open.header.arg0;
    write_frame(socket, adbcpp::protocol::kOkay, kDeviceId, remote);

    write_frame(socket, adbcpp::protocol::kWrte, kDeviceId, remote, bytes_of(output));
    // The client acknowledges the WRITE before the CLOSE, and that OKAY is still
    // unread here; reading it keeps the stream synchronized.
    const auto okay = read_frame(socket);
    if (okay.header.command != adbcpp::protocol::kOkay)
    {
        std::exit(1);
    }
    write_frame(socket, adbcpp::protocol::kClse, kDeviceId, remote);

    std::array<std::byte, 1024> scratch{};
    while (::recv(socket, reinterpret_cast<char *>(scratch.data()), static_cast<int>(scratch.size()), 0) > 0)
    {
    }
}

} // namespace

int main()
{
#if defined(_WIN32)
    const SocketRuntime runtime;
#endif

    // Two devices, each faked by a loopback listener on its own thread. Each
    // thread only writes to its own socket, so the two never share state.
    const std::array<std::string, 2> outputs{"first\n", "second\n"};
    std::array<Loopback, 2> loopbacks;
    std::array<std::thread, 2> servers;
    for (std::size_t i = 0; i < 2; ++i)
    {
        servers[i] = std::thread(
            [&loopbacks, &outputs, i]
            {
                const socket_t socket = loopbacks[i].accept();
                serve_device(socket, outputs[i]);
                close_socket(socket);
            });
    }

    // The transports are opened here and borrowed by the connections, which the
    // streams in turn borrow, so they must outlive both and are declared first.
    std::vector<std::optional<adbcpp::tcp::TcpTransport>> transports(2);
    std::vector<std::optional<adbcpp::Connection>> connections(2);
    std::vector<std::optional<adbcpp::Stream>> streams(2);
    std::vector<adbcpp::Transport *> borrowed;

    for (std::size_t i = 0; i < 2; ++i)
    {
        auto transport = adbcpp::tcp::TcpTransport::open(loopbacks[i].endpoint());
        if (!transport)
        {
            std::cerr << "error: " << transport.error().message << '\n';
            return 1;
        }
        transports[i].emplace(std::move(*transport));

        auto connection = adbcpp::Connection::connect(*transports[i]);
        if (!connection)
        {
            std::cerr << "error: " << connection.error().message << '\n';
            return 1;
        }
        connections[i].emplace(std::move(*connection));

        auto stream = adbcpp::Stream::open(*connections[i], "shell:echo hello");
        if (!stream)
        {
            std::cerr << "error: " << stream.error().message << '\n';
            return 1;
        }
        streams[i].emplace(std::move(*stream));

        borrowed.push_back(&*transports[i]);
    }

    // One thread drives both devices: it waits on both transports at once and
    // reads the one that has bytes, until both streams are done.
    std::vector<bool> done(2, false);
    while (!done[0] || !done[1])
    {
        const auto readable = adbcpp::wait_readable(borrowed, std::chrono::milliseconds(5000));
        if (!readable)
        {
            std::cerr << "error: " << readable.error().message << '\n';
            return 1;
        }
        if (!*readable)
        {
            std::cerr << "timed out waiting for a device\n";
            return 1;
        }

        // The readable stream has bytes, so `read_all` does not wait for the
        // other device. It runs to the CLOSE, so a device that produces output
        // slowly is not interleaved mid-command; a caller that needs that reads
        // frame by frame instead.
        const std::size_t index = **readable;
        const auto output = streams[index]->read_all();
        if (!output)
        {
            std::cerr << "error: " << output.error().message << '\n';
            return 1;
        }
        std::cout << "device " << index
                  << " says: " << std::string(reinterpret_cast<const char *>(output->data()), output->size());
        done[index] = true;
    }

    for (auto &connection : connections)
    {
        connection->close();
    }
    for (auto &server : servers)
    {
        server.join();
    }
    return 0;
}
