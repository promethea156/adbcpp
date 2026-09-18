#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "adbcpp/connection.hpp"
#include "adbcpp/protocol/commands.hpp"
#include "adbcpp/protocol/message.hpp"
#include "adbcpp/shell.hpp"
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

// The TCP transport is exercised against a loopback listener that stands in for
// an emulator's ADB listener, so the whole stack runs with no device attached.
namespace
{

#if defined(_WIN32)
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

void close_socket(socket_t socket) noexcept
{
#if defined(_WIN32)
    ::closesocket(socket);
#else
    ::close(socket);
#endif
}

#if defined(_WIN32)
// The test's own listener needs Winsock before the transport initializes it.
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
        REQUIRE(listener_ != kInvalidSocket);
        const int enabled = 1;
        ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&enabled), sizeof(enabled));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        REQUIRE(::bind(listener_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
        REQUIRE(::listen(listener_, 1) == 0);

        socklen_t size = sizeof(address);
        REQUIRE(::getsockname(listener_, reinterpret_cast<sockaddr *>(&address), &size) == 0);
        port_ = ntohs(address.sin_port);
    }

    ~Loopback()
    {
        if (listener_ != kInvalidSocket)
        {
            close_socket(listener_);
        }
    }

    std::string endpoint() const
    {
        return "127.0.0.1:" + std::to_string(port_);
    }

    socket_t accept() const
    {
        const socket_t socket = ::accept(listener_, nullptr, nullptr);
        REQUIRE(socket != kInvalidSocket);
        return socket;
    }

private:
    socket_t listener_ = kInvalidSocket;
    std::uint16_t port_ = 0;
};

// Fills `buffer` completely, so the test reads a whole frame.
void read_exact(socket_t socket, std::span<std::byte> buffer)
{
    std::size_t total = 0;
    while (total < buffer.size())
    {
        const int received =
            ::recv(socket, reinterpret_cast<char *>(buffer.data()) + total, static_cast<int>(buffer.size() - total), 0);
        REQUIRE(received > 0);
        total += static_cast<std::size_t>(received);
    }
}

struct Frame
{
    adbcpp::protocol::Message header;
    std::vector<std::byte> payload;
};

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
    REQUIRE(::send(socket, reinterpret_cast<const char *>(bytes.data()), static_cast<int>(bytes.size()), 0) ==
            static_cast<int>(bytes.size()));
    if (!payload.empty())
    {
        REQUIRE(::send(socket, reinterpret_cast<const char *>(payload.data()), static_cast<int>(payload.size()), 0) ==
                static_cast<int>(payload.size()));
    }
}

constexpr std::uint32_t kDeviceId = 7;

void write_u32_le(std::byte *out, std::uint32_t value)
{
    out[0] = static_cast<std::byte>(value & 0xFFu);
    out[1] = static_cast<std::byte>((value >> 8) & 0xFFu);
    out[2] = static_cast<std::byte>((value >> 16) & 0xFFu);
    out[3] = static_cast<std::byte>((value >> 24) & 0xFFu);
}

std::span<const std::byte> bytes_of(const std::string &text)
{
    return std::span(reinterpret_cast<const std::byte *>(text.data()), text.size());
}

// Stands in for adbd: answers the CNXN, accepts the OPEN, and writes one
// shell_v2 stdout packet, its exit packet, and a CLOSE.
void serve_device(socket_t socket)
{
    const auto cnxn = read_frame(socket);
    REQUIRE(cnxn.header.command == adbcpp::protocol::kCnxn);

    const std::string banner = "device::features=shell_v2";
    write_frame(socket, adbcpp::protocol::kCnxn, 0x01000001u, 4096u, bytes_of(banner));

    const auto open = read_frame(socket);
    REQUIRE(open.header.command == adbcpp::protocol::kOpen);
    const std::uint32_t remote = open.header.arg0;
    write_frame(socket, adbcpp::protocol::kOkay, kDeviceId, remote);

    const std::string output = "hello\n";
    std::vector<std::byte> packet(5 + output.size());
    packet[0] = std::byte{1};
    write_u32_le(packet.data() + 1, static_cast<std::uint32_t>(output.size()));
    std::copy(bytes_of(output).begin(), bytes_of(output).end(), packet.begin() + 5);
    write_frame(socket, adbcpp::protocol::kWrte, kDeviceId, remote, packet);

    std::vector<std::byte> exit_packet(6);
    exit_packet[0] = std::byte{3};
    write_u32_le(exit_packet.data() + 1, 1u);
    exit_packet[5] = std::byte{0};
    write_frame(socket, adbcpp::protocol::kWrte, kDeviceId, remote, exit_packet);

    write_frame(socket, adbcpp::protocol::kClse, kDeviceId, remote);
}

} // namespace

TEST_CASE("tcp transport moves bytes to a listener", "[tcp]")
{
#if defined(_WIN32)
    const SocketRuntime runtime;
#endif
    const Loopback loopback;

    const std::array<std::byte, 5> reply{std::byte{'w'}, std::byte{'o'}, std::byte{'r'}, std::byte{'l'},
                                         std::byte{'d'}};

    std::thread server(
        [&loopback, &reply]
        {
            const socket_t socket = loopback.accept();
            std::array<std::byte, 5> received{};
            read_exact(socket, received);
            (void)::send(socket, reinterpret_cast<const char *>(reply.data()), static_cast<int>(reply.size()), 0);
            close_socket(socket);
        });

    auto transport = adbcpp::tcp::TcpTransport::open(loopback.endpoint());
    REQUIRE(transport.has_value());

    const std::array<std::byte, 5> payload{std::byte{'h'}, std::byte{'e'}, std::byte{'l'}, std::byte{'l'},
                                           std::byte{'o'}};
    REQUIRE(transport->write(payload).has_value());

    std::array<std::byte, 5> received{};
    const auto count = transport->read(received);
    REQUIRE(count.has_value());
    REQUIRE(*count == 5);
    REQUIRE(received == reply);

    transport->close();
    server.join();
}

TEST_CASE("tcp transport rejects an endpoint without a port", "[tcp]")
{
#if defined(_WIN32)
    const SocketRuntime runtime;
#endif
    const auto transport = adbcpp::tcp::TcpTransport::open("localhost");
    REQUIRE_FALSE(transport.has_value());
    REQUIRE(transport.error().code == adbcpp::ErrorCode::InvalidArgument);
}

TEST_CASE("run works over tcp against a fake device", "[tcp]")
{
#if defined(_WIN32)
    const SocketRuntime runtime;
#endif
    const Loopback loopback;

    std::thread server(
        [&loopback]
        {
            const socket_t socket = loopback.accept();
            serve_device(socket);
            close_socket(socket);
        });

    auto transport = adbcpp::tcp::TcpTransport::open(loopback.endpoint());
    REQUIRE(transport.has_value());

    auto connection = adbcpp::Connection::connect(*transport);
    REQUIRE(connection.has_value());

    const auto result = adbcpp::run(*connection, "echo hello");
    REQUIRE(result.has_value());
    REQUIRE(result->output == "hello\n");
    REQUIRE(result->exit_code == 0);
    REQUIRE(result->success);

    transport->close();
    server.join();
}
