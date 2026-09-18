#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>

#include "adbcpp/connection.hpp"
#include "adbcpp/crypto/adb_key.hpp"
#include "adbcpp/shell.hpp"
#include "adbcpp/sync.hpp"
#include "adbcpp/tcp/tcp_transport.hpp"

// Connects to a device over TCP, which is how an emulator is reached: its ADB
// listener accepts a plain connection on `localhost:5555`. The optional argument
// is the `host:port` endpoint, and the default is an emulator on the local
// machine. The key is shared with adb, so the device does not prompt once it is
// authorized.
int main(int argc, char **argv)
{
    std::string endpoint = "localhost:5555";
    if (argc > 1)
    {
        endpoint = argv[1];
    }

    auto transport = adbcpp::tcp::TcpTransport::open(endpoint);
    if (!transport)
    {
        std::cerr << "error: " << transport.error().message << '\n';
        return 1;
    }

    const auto key = adbcpp::crypto::Key::load_or_generate();
    if (!key)
    {
        std::cerr << "error: " << key.error().message << '\n';
        return 1;
    }
    const std::string &public_key_string = key->public_key();
    const auto public_key =
        std::span(reinterpret_cast<const std::byte *>(public_key_string.data()), public_key_string.size());

    auto connection = adbcpp::Connection::connect(*transport, public_key,
                                                  [&key](std::span<const std::byte> token)
                                                  {
                                                      return key->sign(token);
                                                  });
    if (!connection)
    {
        std::cerr << "error: " << connection.error().message << '\n';
        return 1;
    }
    std::cout << "connected to a device running protocol 0x" << std::hex << connection->device_version() << std::dec
              << '\n';

    const auto hello = adbcpp::run(*connection, "echo hello");
    if (!hello)
    {
        std::cerr << "error: " << hello.error().message << '\n';
        return 1;
    }
    std::cout << hello->output;

    // A file round trip, so the example covers a transfer over TCP too.
    const auto local = std::filesystem::temp_directory_path() / "adbcpp_tcp_example.txt";
    {
        std::ofstream output(local, std::ios::binary | std::ios::trunc);
        output << "adbcpp tcp test\n";
    }
    const std::string remote = "/data/local/tmp/adbcpp_tcp_example.txt";
    if (const auto status = adbcpp::push(*connection, local, remote); !status)
    {
        std::cerr << "error: " << status.error().message << '\n';
        return 1;
    }
    std::filesystem::remove(local);

    const auto pulled = std::filesystem::temp_directory_path() / "adbcpp_tcp_back.txt";
    if (const auto status = adbcpp::pull(*connection, remote, pulled); !status)
    {
        std::cerr << "error: " << status.error().message << '\n';
        return 1;
    }
    std::string contents;
    {
        std::ifstream input(pulled, std::ios::binary);
        contents.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    std::filesystem::remove(pulled);
    (void)adbcpp::run(*connection, "rm -f " + remote);
    if (contents != "adbcpp tcp test\n")
    {
        std::cerr << "error: the round-tripped file differs\n";
        return 1;
    }
    std::cout << "round-tripped a file over TCP\n";

    transport->close();
    return 0;
}
