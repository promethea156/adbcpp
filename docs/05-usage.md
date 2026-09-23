# Usage

Practical, copy-pasteable examples for everything `adbcpp` can do today: connect
over USB or TCP, run a shell command, list a directory, pull and push files, stat a path,
install and uninstall an application, launch, close, and check an application, log
what the library is doing, work with the ADB key, and use the lower-level protocol
layers directly. Every example compiles against the library as it stands now. For the theory behind them, read
[`LEARNING.md`](../LEARNING.md); for how the `sync` service works, read
[`06-sync-protocol.md`](06-sync-protocol.md).

## Linking the Library

`adbcpp` is a normal CMake project. The simplest way to consume it is to add it
with `FetchContent` and link the targets you need:

```cmake
include(FetchContent)
FetchContent_Declare(
  adbcpp
  GIT_REPOSITORY https://github.com/promethea156/adbcpp.git
  GIT_TAG main
)
FetchContent_MakeAvailable(adbcpp)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE adbcpp::adbcpp)
```

There are three targets:

| Target           | What it adds                                  | Extra dependency |
| ---------------- | --------------------------------------------- | ---------------- |
| `adbcpp::adbcpp` | Protocol, session, streams, shell, sync, mock transport | `tl::expected`, zstd, lz4, brotli |
| `adbcpp::crypto` | `adbcpp::crypto::Key` (ADB key pair)           | mbedTLS          |
| `adbcpp::usb`    | `adbcpp::usb::UsbTransport` (USB transport)    | libusb           |

`adbcpp::crypto` and `adbcpp::usb` both link `adbcpp::adbcpp` publicly, so
linking the USB backend pulls in the rest. `adbcpp::usb` is only defined when the
project is built with `ADBCPP_BUILD_USB=ON` (the default at the top level). The TCP
transport has no third-party dependency, so it lives in `adbcpp::adbcpp` and is always
available. `adbcpp::adbcpp` links zstd (`ADBCPP_BUILD_COMPRESSION`), lz4
(`ADBCPP_BUILD_LZ4`), and brotli (`ADBCPP_BUILD_BROTLI`) privately, so the codec
headers stay out of the public headers; any of them is off for a build that does not
want that codec.

Alternatively, install `adbcpp` and use `find_package`:

```
cmake --install build --config Release --prefix /some/prefix
```

```cmake
find_package(adbcpp REQUIRED)
target_link_libraries(my_app PRIVATE adbcpp::crypto adbcpp::usb)
```

All three targets are exported, and the install includes the mbedTLS and libusb
libraries they need. On Windows the libusb DLL lands in `<prefix>/bin`, so add that
directory to `PATH` (or copy it next to your executable) at runtime.

Include what you use:

```cpp
#include "adbcpp/adbcpp.hpp"                 // core: Connection, Stream, run, install, protocol
#include "adbcpp/app.hpp"                     // adbcpp::install, uninstall, launch, close, is_running
#include "adbcpp/log.hpp"                     // adbcpp::set_logger, adbcpp::LogLevel
#include "adbcpp/sync.hpp"                    // adbcpp::list, adbcpp::DirEntry
#include "adbcpp/crypto/adb_key.hpp"          // adbcpp::crypto::Key
#include "adbcpp/tcp/tcp_transport.hpp"       // adbcpp::tcp::TcpTransport
#include "adbcpp/usb/usb_transport.hpp"       // adbcpp::usb::UsbTransport
#include "adbcpp/testing/mock_transport.hpp"   // adbcpp::testing::MockTransport
```

## The Error Model

`adbcpp` does not throw for its own failures. Every operation that can fail returns a
`Result<T>`: the value when it worked, or the `Error` explaining why it did not. The
caller checks the result and decides what to do with it. The design is described in full
in [`07-error-model.md`](07-error-model.md).

Every operation is in one of two cases, and the shape of its result follows the case.
The distinction is not "severe versus mild", it is **who is being reported on**:

- **The device answered.** The answer is the value: a command's output and exit code, a
  question's answer, or nothing at all for an operation with nothing to report. A `stat` of
  a path that does not exist is this case, because the device answered that there is
  nothing there, so `stat` returns an empty `std::optional`.
- **The operation could not be carried out.** The transport failed, the stream failed, the
  device did not answer as the protocol requires, or it refused the request with a reason.
  That is an `Error`, returned as the unexpected value of the `Result`.

```cpp
// include/adbcpp/error.hpp
enum class ErrorCode { InvalidArgument, Transport, Protocol, Device, Crypto, Io };

struct Error
{
    ErrorCode code = ErrorCode::Protocol;
    std::string message;
};

template <typename T>
using Result = tl::expected<T, Error>;
using Status = tl::expected<void, Error>;
```

```cpp
// include/adbcpp/shell.hpp
struct CommandResult
{
    std::string output;          // stdout and stderr, combined in order
    std::string standard_output;  // stdout alone; empty under v1
    std::string error_output;    // stderr alone; empty under v1
    std::uint8_t exit_code = 0;
    bool success = false;        // set per command; see below
};
```

`Result` is `tl::expected` itself, so `has_value()`, `operator*`, `operator->`, and
`value_or` are available. Check with `has_value()` or `operator bool`, then unwrap with
`operator*` or `operator->`; `value()` and `error()` assert on the wrong alternative and
must not be called before checking. A caller prints `error: <message>`.

```cpp
const auto result = adbcpp::run(connection, "echo hello");
if (!result)
{
    std::cerr << "error: " << result.error().message << '\n';
    return 1;
}
std::cout << result->output;
```

A device's rejection is an answer rather than an `Error`. `install` and `uninstall` set
`CommandResult::success` to `exit_code == 0` **and** the output says `Success`, because
`pm` reports a rejection in its output. A package manager that refuses an APK is therefore
reported with `success == false` and its output, while a failed transfer or stream is an
`Error`.

## Log What the Library Is Doing

Logging is opt-in and off by default. `adbcpp::set_logger` installs a sink and a
level, and the library then writes protocol events to it, from a frame sent or received
to a retry or a state change. `adbcpp::clear_logger` removes it again.

```cpp
#include <iostream>
#include <string_view>

#include "adbcpp/adbcpp.hpp"

int main()
{
    // `Debug` adds the frames to the state changes; `Trace` adds the service a
    // stream is opened for. The default is `Info`.
    adbcpp::set_logger(
        [](adbcpp::LogLevel level, std::string_view message) { std::cerr << message << '\n'; },
        adbcpp::LogLevel::Debug);

    // ... connect and run a command; the CNXN/AUTH/OPEN/WRTE frames are written.

    adbcpp::clear_logger();
    return 0;
}
```

The sink is process-wide, because the objects that log do not take one of their own.
It is called from the thread that caused the event, so with one thread per device it
may be called from several threads at once and must serialize itself if it shares
state. It must not throw, because the library reports its failures with `Result`.

The levels are ordered, and a message is written when it is at or below the installed
level:

| Level | What it reports |
| --- | --- |
| `Error` | A failure, which the operation also returns as an `Error`. |
| `Warning` | Something unexpected that did not fail, such as a retry. |
| `Info` | A state change: a connection, a stream, an authorization. This is the default. |
| `Debug` | A protocol frame sent or received, with its command, arguments, and length. |
| `Trace` | A payload-derived detail, such as the service a stream is opened for. |

The library never logs key material or a payload: a frame is logged with its command,
its arguments, and its length only, and the service is the only payload-derived detail.
`tests/log_test.cpp` pins that down by driving the handshake with a distinctive token,
signature, and public key and checking that none of them reaches the sink.

## Run a Shell Command over USB

This is the main use case: connect directly to a device over USB and run a command,
without an `adb` server or the `adb` binary.

```cpp
#include <iostream>
#include <optional>
#include <span>
#include <string>

#include "adbcpp/adbcpp.hpp"
#include "adbcpp/crypto/adb_key.hpp"
#include "adbcpp/usb/usb_transport.hpp"

int main()
{
    // Every Android device exposes its ADB function with the same USB ids.
    adbcpp::usb::DeviceId id;
    id.vendor_id = 0x22D9;
    id.product_id = 0x2769;

    // Opening a device that is absent (or claimed by a running adb server) fails,
    // so check for it first.
    const auto present = adbcpp::usb::UsbTransport::is_present(id);
    if (!present)
    {
        std::cerr << "error: " << present.error().message << '\n';
        return 1;
    }
    if (!*present)
    {
        std::cerr << "no matching USB device found\n";
        return 1;
    }

    // Reuse adb's key, so the device does not show the approval prompt. The key
    // must outlive the connection, because the signer below captures it.
    const auto key = adbcpp::crypto::Key::load_or_generate();
    if (!key)
    {
        std::cerr << "error: " << key.error().message << '\n';
        return 1;
    }
    const std::string &public_key_string = key->public_key();
    const auto public_key = std::span(reinterpret_cast<const std::byte *>(public_key_string.data()),
                                     public_key_string.size());

    // `connect_with_retry` opens the transport and performs the CNXN/AUTH
    // handshake, retrying the whole open and handshake because a USB 3 device can
    // reset its link right after the open (blocker 29). The opened transport
    // lives in `transport`, which the connection borrows, so it must outlive the
    // connection.
    std::optional<adbcpp::usb::UsbTransport> transport;
    auto connection = adbcpp::connect_with_retry([&id] { return adbcpp::usb::UsbTransport::open(id); }, transport,
                                              public_key,
                                              [&key](std::span<const std::byte> token) { return key->sign(token); });
    if (!connection)
    {
        std::cerr << "error: " << connection.error().message << '\n';
        return 1;
    }

    const auto result = adbcpp::run(*connection, "echo hello");
    if (!result)
    {
        std::cerr << "error: " << result.error().message << '\n';
        return 1;
    }

    std::cout << "exit code: " << static_cast<int>(result->exit_code) << '\n';
    std::cout << result->output;

    connection->close();
    return result->exit_code;
}
```

`CommandResult::output` is stdout and stderr combined, in the order the device
produced them, and `exit_code` is the command's status. `standard_output` and
`error_output` report each stream on its own under the `shell_v2` service; under the
v1 `shell` service, which does not separate them, the two are empty and `output` is
its raw stream.

`connection->close()` closes the borrowed transport and marks the connection dead. The
connection does not own the transport, so the transport must outlive it, but a caller
does not have to close the transport separately: `close()` does that, and a later
`send` or `receive` reports a `Transport` error instead of touching the closed
transport. `Connection::is_open()` says whether `close()` has been called.

`run` uses the `shell,v2,raw` service when the device advertised `shell_v2`, and the
v1 `shell:<command>` service otherwise. The v1 form has no exit packet, so
`exit_code` is always 0, exactly as in adb; `ShellProtocol::V1` and
`ShellProtocol::V2` force one form or the other:

```cpp
// Force the v1 service, whose output is raw and whose exit code is always 0.
const auto result = adbcpp::run(*connection, "echo hello", adbcpp::ShellProtocol::V1);
```

Two devices of the same model share the vendor and product id, so they are told
apart by their USB serial. `UsbTransport::list` returns every attached ADB device
with its serial, and `DeviceId::parse` accepts either a `VID:PID` model or a
`serial:<serial>` selector:

```cpp
const auto devices = adbcpp::usb::UsbTransport::list();
if (!devices)
{
    std::cerr << "error: " << devices.error().message << '\n';
    return 1;
}
for (const auto &device : *devices)
{
    std::cout << std::hex << device.vendor_id << ':' << device.product_id << std::dec << ' ' << device.serial << '\n';
}

const auto id = adbcpp::usb::DeviceId::parse("serial:3B15AD001NS00000");
if (!id)
{
    std::cerr << "error: " << id.error().message << '\n';
    return 1;
}
auto transport = adbcpp::usb::UsbTransport::open(*id);
```

`Connection::device_serial` returns the device's serial, which is the transport's USB
`iSerial` descriptor or TCP endpoint, the same string `adb devices` prints. It falls back
to the banner's `serialno` field for a device whose transport has none, which is empty on
most current devices:

```cpp
std::cout << "serial: " << connection->device_serial() << '\n';
```

## Connect over TCP

An emulator, or a device put into `tcpip` mode, is reached over TCP instead of
USB. `TcpTransport::open` takes a `host:port` endpoint and implements the same
`Transport` interface as `UsbTransport`, so every example below works unchanged;
only the transport line differs.

```cpp
#include <iostream>
#include <span>
#include <string>

#include "adbcpp/adbcpp.hpp"
#include "adbcpp/crypto/adb_key.hpp"
#include "adbcpp/tcp/tcp_transport.hpp"

int main()
{
    // An emulator listens on `localhost:5555` by default. A device reached over
    // the network uses its own `host:port`.
    auto transport = adbcpp::tcp::TcpTransport::open("localhost:5555");
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
    const auto public_key = std::span(reinterpret_cast<const std::byte *>(public_key_string.data()),
                                     public_key_string.size());

    auto connection = adbcpp::Connection::connect(*transport, public_key,
                                  [&key](std::span<const std::byte> token) { return key->sign(token); });
    if (!connection)
    {
        std::cerr << "error: " << connection.error().message << '\n';
        return 1;
    }

    const auto result = adbcpp::run(*connection, "echo hello");
    if (!result)
    {
        std::cerr << "error: " << result.error().message << '\n';
        return 1;
    }
    std::cout << result->output;

    connection->close();
    return result->exit_code;
}
```

The endpoint is `host:port`, where `host` is a name or a literal address and
`port` is a service name or a number, so `localhost:5555`, `127.0.0.1:5555`, and
`[::1]:5555` all work. The transport has no libusb dependency, so it needs no
`ADBCPP_BUILD_USB`. Put a device into TCP mode with `adb tcpip 5555`, then connect
directly with no adb server.

The lifecycle is the same as over USB: the connection borrows the transport, so the
transport must outlive it, and `connection->close()` closes it and marks the
connection dead.

## Reconnect a Dropped Link

A dropped TCP link, a USB 3 link reset, or a replug leaves a connection whose
transport is dead. `adbcpp::connect_with_retry` opens a transport with a callable
and performs the handshake, retrying the whole open and handshake with a bounded
exponential backoff (blocker 29), so the recovery is not hand-rolled at each call
site:

```cpp
#include <iostream>
#include <optional>
#include <span>
#include <string>

#include "adbcpp/adbcpp.hpp"
#include "adbcpp/crypto/adb_key.hpp"
#include "adbcpp/tcp/tcp_transport.hpp"

int main()
{
    const auto key = adbcpp::crypto::Key::load_or_generate();
    if (!key)
    {
        std::cerr << "error: " << key.error().message << '\n';
        return 1;
    }
    const std::string &public_key_string = key->public_key();
    const auto public_key = std::span(reinterpret_cast<const std::byte *>(public_key_string.data()),
                                     public_key_string.size());

    // `open` is called before each attempt and returns a fresh transport.
    const auto open = [] { return adbcpp::tcp::TcpTransport::open("localhost:5555"); };

    std::optional<adbcpp::tcp::TcpTransport> transport;
    auto connection = adbcpp::connect_with_retry(open, transport, public_key,
                                              [&key](std::span<const std::byte> token) { return key->sign(token); });
    if (!connection)
    {
        std::cerr << "error: " << connection.error().message << '\n';
        return 1;
    }

    const auto result = adbcpp::run(*connection, "echo hello");
    if (!result)
    {
        std::cerr << "error: " << result.error().message << '\n';
        return 1;
    }
    std::cout << result->output;

    // The link dropped. Close the connection and call the helper again: it
    // replaces the transport in `transport` and repeats the handshake.
    connection->close();
    connection = adbcpp::connect_with_retry(open, transport, public_key,
                                           [&key](std::span<const std::byte> token) { return key->sign(token); });
    if (!connection)
    {
        std::cerr << "error: " << connection.error().message << '\n';
        return 1;
    }

    connection->close();
    return 0;
}
```

The second attempt waits `250 ms`, and each later attempt doubles the wait, so
five attempts span about four seconds and stay finite. `transport` is where the
opened transport lives and must outlive the connection, so it is the caller's, not
the helper's.

## Drive Several Devices from One Thread

The default model for several devices is one thread per device, which needs no
coordination. When one thread must drive several, `adbcpp::wait_readable` waits on
several transports at once and returns the index of one that is readable; that
transport's next `read` does not block.

```cpp
#include <chrono>
#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "adbcpp/adbcpp.hpp"
#include "adbcpp/tcp/tcp_transport.hpp"

int main()
{
    // Two transports to two devices, or to two loopback listeners as here.
    std::vector<std::optional<adbcpp::tcp::TcpTransport>> transports(2);
    std::vector<std::optional<adbcpp::Connection>> connections(2);
    std::vector<std::optional<adbcpp::Stream>> streams(2);
    std::vector<adbcpp::Transport *> borrowed;

    for (std::size_t i = 0; i < 2; ++i)
    {
        const std::string endpoint = ...;
        auto transport = adbcpp::tcp::TcpTransport::open(endpoint);
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

        // The command is the service string, so nothing is written.
        auto stream = adbcpp::Stream::open(*connections[i], "shell:echo hello");
        if (!stream)
        {
            std::cerr << "error: " << stream.error().message << '\n';
            return 1;
        }
        streams[i].emplace(std::move(*stream));

        borrowed.push_back(&*transports[i]);
    }

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

        // The readable transport has bytes, so this read does not wait on the
        // other device.
        const std::size_t index = **readable;
        const auto output = streams[index]->read_all();
        if (!output)
        {
            std::cerr << "error: " << output.error().message << '\n';
            return 1;
        }
        std::cout << "device " << index << " says: "
                  << std::string(reinterpret_cast<const char *>(output->data()), output->size());
        done[index] = true;
    }

    for (auto &connection : connections)
    {
        connection->close();
    }
    return 0;
}
```

`wait_readable` returns the index of a readable transport, or nothing when the
timeout passes with none readable. `TcpTransport` waits with `select`, `UsbTransport`
with one bulk transfer bounded by the timeout, and the mock reports whether bytes are
queued. A transport that cannot wait, such as a caller's own, is never returned.

`examples/poll` runs this shape over two loopback listeners and is registered in CI,
so the feature is exercised with no device attached. The transports must outlive the
connections and the connections the streams, so they are declared first.

`read_all` reads one device to its `CLOSE`, so a device that produces output slowly
is not interleaved mid-command. A caller that needs that reads frame by frame
instead.

## List a Directory

`list` opens the `sync:` service and returns a directory's entries as structured
data. It uses the v2 `LIST`/`DNT2` form when the device advertises `ls_v2`, which
most modern devices do.

```cpp
#include <iostream>
#include <span>
#include <string>

#include "adbcpp/adbcpp.hpp"
#include "adbcpp/crypto/adb_key.hpp"
#include "adbcpp/sync.hpp"
#include "adbcpp/usb/usb_transport.hpp"

int main()
{
    adbcpp::usb::DeviceId id;
    id.vendor_id = 0x22D9;
    id.product_id = 0x2769;

    auto transport = adbcpp::usb::UsbTransport::open(id);
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
    const auto public_key = std::span(reinterpret_cast<const std::byte *>(public_key_string.data()),
                                     public_key_string.size());

    auto connection = adbcpp::Connection::connect(*transport, public_key,
                                  [&key](std::span<const std::byte> token) { return key->sign(token); });
    if (!connection)
    {
        std::cerr << "error: " << connection.error().message << '\n';
        return 1;
    }

    const auto entries = adbcpp::list(*connection, "/sdcard");
    if (!entries)
    {
        std::cerr << "error: " << entries.error().message << '\n';
        return 1;
    }
    for (const auto &entry : *entries)
    {
        std::cout << (entry.is_directory() ? 'd' : '-') << ' ' << entry.size << ' ' << entry.name << '\n';
    }

    connection->close();
    return 0;
}
```

`DirEntry` carries the POSIX metadata the device reports, the same values
`lstat` would give: `name`, `mode`, `size`, and `mtime`, plus `is_directory()`
and `is_regular()`. A file larger than 4 GiB is reported correctly, because the
v2 entry form uses a 64-bit size.

### List a Directory Without a Device

To run `list`, `pull`, and `push` with no device attached, see
[`examples/sync/main.cpp`](../examples/sync/main.cpp). It queues a device's CNXN and
its `DNT2`, `DATA`, `DONE`, `STA2`, and `OKAY` responses on the mock transport,
calls the real `list`, `pull`, and `push`, prints the entries and the pulled
contents, and then prints the `LIS2`, `RECV`, and `SEND` request headers that they
wrote. It is registered with CTest as `example_sync` and runs in CI. The wire format
itself is explained in [`06-sync-protocol.md`](06-sync-protocol.md).

## Pull a File

`pull` copies a file from the device to a local path. It writes each `DATA` chunk as
it arrives, so the file is never held in memory whole and a large file costs no more
memory than a small one.

`pull` and `push` take a `SyncCompression`, and the default `Auto` uses the v2
`RECV`/`SEND` forms with the best codec both sides have, in adb's order (zstd, then
lz4, then brotli), and the v1 forms otherwise. `None` always uses the v1 forms, and
`Zstd`, `Lz4`, or `Brotli` requires the device to have advertised that codec.

```cpp
#include <filesystem>
#include <iostream>
#include <span>
#include <string>

#include "adbcpp/adbcpp.hpp"
#include "adbcpp/crypto/adb_key.hpp"
#include "adbcpp/sync.hpp"
#include "adbcpp/usb/usb_transport.hpp"

int main()
{
    adbcpp::usb::DeviceId id;
    id.vendor_id = 0x22D9;
    id.product_id = 0x2769;

    auto transport = adbcpp::usb::UsbTransport::open(id);
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
    const auto public_key = std::span(reinterpret_cast<const std::byte *>(public_key_string.data()),
                                     public_key_string.size());

    auto connection = adbcpp::Connection::connect(*transport, public_key,
                                  [&key](std::span<const std::byte> token) { return key->sign(token); });
    if (!connection)
    {
        std::cerr << "error: " << connection.error().message << '\n';
        return 1;
    }

    // The local file is created or truncated, and its directory must exist.
    const auto pulled = adbcpp::pull(*connection, "/sdcard/Download/report.pdf", "report.pdf");
    if (!pulled)
    {
        std::cerr << "error: " << pulled.error().message << '\n';
        return 1;
    }

    connection->close();
    return 0;
}
```

A transfer that fails part way through leaves the partial file in place, so the
caller can decide whether to retry or remove it.

## Push a File

`push` copies a local file to the device. It sends the file in 64 KiB chunks, so the
file is never held in memory whole.

```cpp
#include <filesystem>
#include <iostream>
#include <span>
#include <string>

#include "adbcpp/adbcpp.hpp"
#include "adbcpp/crypto/adb_key.hpp"
#include "adbcpp/sync.hpp"
#include "adbcpp/usb/usb_transport.hpp"

int main()
{
    adbcpp::usb::DeviceId id;
    id.vendor_id = 0x22D9;
    id.product_id = 0x2769;

    auto transport = adbcpp::usb::UsbTransport::open(id);
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
    const auto public_key = std::span(reinterpret_cast<const std::byte *>(public_key_string.data()),
                                     public_key_string.size());

    auto connection = adbcpp::Connection::connect(*transport, public_key,
                                  [&key](std::span<const std::byte> token) { return key->sign(token); });
    if (!connection)
    {
        std::cerr << "error: " << connection.error().message << '\n';
        return 1;
    }

    // An existing directory receives the file under its local name.
    const auto pushed = adbcpp::push(*connection, "report.pdf", "/sdcard/Download/");
    if (!pushed)
    {
        std::cerr << "error: " << pushed.error().message << '\n';
        return 1;
    }

    connection->close();
    return 0;
}
```

The device creates the destination, or overwrites it if it already exists, with the
local file's permissions and modification time.

## Stat a Path

`stat` reports a path's metadata, following symbolic links. A path that does not
exist is an empty `std::optional` rather than an `Error`.

```cpp
#include <iostream>
#include <span>
#include <string>

#include "adbcpp/adbcpp.hpp"
#include "adbcpp/crypto/adb_key.hpp"
#include "adbcpp/sync.hpp"
#include "adbcpp/usb/usb_transport.hpp"

int main()
{
    adbcpp::usb::DeviceId id;
    id.vendor_id = 0x22D9;
    id.product_id = 0x2769;

    auto transport = adbcpp::usb::UsbTransport::open(id);
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
    const auto public_key = std::span(reinterpret_cast<const std::byte *>(public_key_string.data()),
                                     public_key_string.size());

    auto connection = adbcpp::Connection::connect(*transport, public_key,
                                  [&key](std::span<const std::byte> token) { return key->sign(token); });
    if (!connection)
    {
        std::cerr << "error: " << connection.error().message << '\n';
        return 1;
    }

    const auto info = adbcpp::stat(*connection, "/sdcard/Download/report.pdf");
    if (!info)
    {
        std::cerr << "error: " << info.error().message << '\n';
        return 1;
    }
    if (*info)
    {
        std::cout << ((*info)->is_directory() ? 'd' : '-') << ' ' << (*info)->size << '\n';
    }
    else
    {
        std::cout << "no such path\n";
    }

    connection->close();
    return 0;
}
```

## Install and Uninstall an Application

`install` pushes an APK into the device's `/data/local/tmp`, installs it from there
with `pm install`, and removes the pushed copy. Both of the services it needs are
already in the library, so this is composition rather than a new protocol: `push` is
the `sync` `SEND` request, and `pm install` runs over the shell service.

```cpp
#include <iostream>
#include <span>
#include <string>

#include "adbcpp/adbcpp.hpp"
#include "adbcpp/crypto/adb_key.hpp"
#include "adbcpp/usb/usb_transport.hpp"

int main()
{
    adbcpp::usb::DeviceId id;
    id.vendor_id = 0x22D9;
    id.product_id = 0x2769;

    auto transport = adbcpp::usb::UsbTransport::open(id);
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
    const auto public_key = std::span(reinterpret_cast<const std::byte *>(public_key_string.data()),
                                     public_key_string.size());

    auto connection = adbcpp::Connection::connect(*transport, public_key,
                                  [&key](std::span<const std::byte> token) { return key->sign(token); });
    if (!connection)
    {
        std::cerr << "error: " << connection.error().message << '\n';
        return 1;
    }

    // `-r` replaces an existing package and keeps its data.
    const auto installed = adbcpp::install(*connection, "app.apk", "-r");
    if (!installed)
    {
        std::cerr << "error: " << installed.error().message << '\n';
        return 1;
    }
    if (!installed->success)
    {
        std::cerr << "install failed: " << installed->failure_reason() << '\n';
    }

    const auto removed = adbcpp::uninstall(*connection, "com.example.app");
    if (!removed)
    {
        std::cerr << "error: " << removed.error().message << '\n';
        return 1;
    }
    if (!removed->success)
    {
        std::cerr << "uninstall failed: " << removed->failure_reason() << '\n';
    }

    connection->close();
    return 0;
}
```

`options` are passed to `pm install` verbatim, so `-r` replaces an existing
package and keeps its data, `-d` allows a version downgrade, and `-g` grants all
runtime permissions. `uninstall(connection, "com.example.app", true)` adds `-k` to
`pm uninstall`, which keeps the package's data and cache directories.

A package manager that rejects the request is a normal answer rather than an error, so
both return `success == false` with the device's output instead of an `Error`. An
`Error` means the transfer or the stream failed, not the install.
`PackageResult::failure_reason()` extracts the reason from `Failure [REASON]`; the
device does not always answer in that form, so `output` always holds its answer
verbatim (blocker 25).

## Launch, Close, and Check an App

`launch` runs `monkey -p <package> -c android.intent.category.LAUNCHER 1`;
`close` runs `am force-stop <package>`; and `is_running` runs `pidof <package>`.
All are shell commands, so they are composition rather than a new protocol.

```cpp
#include <iostream>
#include <span>
#include <string>

#include "adbcpp/adbcpp.hpp"
#include "adbcpp/crypto/adb_key.hpp"
#include "adbcpp/usb/usb_transport.hpp"

int main()
{
    adbcpp::usb::DeviceId id;
    id.vendor_id = 0x22D9;
    id.product_id = 0x2769;

    auto transport = adbcpp::usb::UsbTransport::open(id);
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
    const auto public_key = std::span(reinterpret_cast<const std::byte *>(public_key_string.data()),
                                     public_key_string.size());

    auto connection = adbcpp::Connection::connect(*transport, public_key,
                                  [&key](std::span<const std::byte> token) { return key->sign(token); });
    if (!connection)
    {
        std::cerr << "error: " << connection.error().message << '\n';
        return 1;
    }

    const auto launched = adbcpp::launch(*connection, "com.example.app");
    if (!launched)
    {
        std::cerr << "error: " << launched.error().message << '\n';
        return 1;
    }
    if (!launched->success)
    {
        std::cerr << "launch failed: " << launched->output << '\n';
    }

    const auto running = adbcpp::is_running(*connection, "com.example.app");
    if (!running)
    {
        std::cerr << "error: " << running.error().message << '\n';
        return 1;
    }
    std::cout << (*running ? "running\n" : "not running\n");

    if (const auto status = adbcpp::close(*connection, "com.example.app"); !status)
    {
        std::cerr << "error: " << status.error().message << '\n';
        return 1;
    }

    connection->close();
    return 0;
}
```

`launch` sets `success` from the `monkey` exit code, so a package whose launcher
cannot be started is a normal `success == false` with `monkey`'s answer as the
output.
`close` returns a `Status` because `am force-stop` exits zero even for a package
that is not installed and prints nothing, so there is no per-command answer to
inspect. `is_running` returns a `Result<bool>`: `pidof` exits zero with the pids
when the process runs and nonzero with no output when it does not, so "not running"
is a definite `false`. `pidof` matches a process name rather than a package name,
so an application that renames its process makes the answer an approximation.

## Inspect the ADB Key

The key lives in `~/.android/adbkey` (PKCS#8 PEM) and `~/.android/adbkey.pub`
(ADB's custom public key format). `fingerprint()` returns the MD5 fingerprint that
the device shows in its **USB debugging authorized computers** list, so you can check
that both tools use the same key.

```cpp
#include <iostream>

#include "adbcpp/crypto/adb_key.hpp"

int main()
{
    // Loads the existing key, or generates one on first use.
    const auto key = adbcpp::crypto::Key::load_or_generate();
    if (!key)
    {
        std::cerr << "error: " << key.error().message << '\n';
        return 1;
    }

    const auto fingerprint = key->fingerprint();
    if (!fingerprint)
    {
        std::cerr << "error: " << fingerprint.error().message << '\n';
        return 1;
    }

    std::cout << "fingerprint: " << *fingerprint << '\n';
    std::cout << "public key:  " << key->public_key() << '\n';
    return 0;
}
```

> The fingerprint here is the MD5 of the decoded 524-byte blob, which is what the
> device shows. `adb`'s own log prints a SHA-256 of the DER
> `SubjectPublicKeyInfo`, so the two fingerprints are different on purpose.

## Work Without a Device

`adbcpp::testing::MockTransport` is a `Transport` backed by two byte buffers, so
the whole protocol stack can be exercised with no device at all. `feed()` queues
bytes to be read, and `written()` returns what the library wrote.

```cpp
#include <array>
#include <iostream>

#include "adbcpp/adbcpp.hpp"
#include "adbcpp/testing/mock_transport.hpp"

int main()
{
    adbcpp::testing::MockTransport transport;

    // Queue the device's CNXN, exactly as it would arrive on the wire.
    adbcpp::protocol::Message device;
    device.command = adbcpp::protocol::kCnxn;
    device.arg0 = adbcpp::protocol::kVersion;
    device.arg1 = adbcpp::protocol::kMaxData;
    device.magic = adbcpp::protocol::Message::compute_magic(device.command);
    transport.feed(device.encode());

    // The connection performs the handshake against the queued messages.
    const auto connection = adbcpp::Connection::connect(transport);
    if (!connection)
    {
        std::cerr << "error: " << connection.error().message << '\n';
        return 1;
    }

    std::cout << "device version: 0x" << std::hex << connection->device_version() << '\n';
    std::cout << "device max data: " << std::dec << connection->max_data() << '\n';
    return 0;
}
```

`tests/tcp_test.cpp` does the same over a real loopback socket: a background
listener on `127.0.0.1` stands in for an emulator's ADB listener, so the TCP
transport, the handshake, and `run` are all exercised with no device attached and
in CI.

## Open a Service Manually

`Stream` is the layer below `run`: it opens a named service and lets you write
to it and read its output. `run` and `list` are both built on it.

```cpp
#include <cstddef>
#include <iostream>
#include <span>
#include <string>

#include "adbcpp/adbcpp.hpp"
#include "adbcpp/testing/mock_transport.hpp"

int main()
{
    adbcpp::testing::MockTransport transport;

    adbcpp::protocol::Message device;
    device.command = adbcpp::protocol::kCnxn;
    device.arg0 = adbcpp::protocol::kVersion;
    device.arg1 = adbcpp::protocol::kMaxData;
    device.magic = adbcpp::protocol::Message::compute_magic(device.command);
    transport.feed(device.encode());

    // The device accepts the OPEN. arg0 is its id for the stream; we chose id 2.
    adbcpp::protocol::Message okay;
    okay.command = adbcpp::protocol::kOkay;
    okay.arg0 = 7;
    okay.arg1 = 2;
    okay.magic = adbcpp::protocol::Message::compute_magic(okay.command);
    transport.feed(okay.encode());

    // The device writes "hello\n" and then closes its side. The ids are relative
    // to the sender, so `arg0` is the device's id for the stream (7) and `arg1`
    // is ours (2). Our own frames are the other way round.
    const std::string text = "hello\n";
    const auto payload = std::span(reinterpret_cast<const std::byte *>(text.data()), text.size());
    const auto length = adbcpp::protocol::Message::data_length_of(payload);
    if (!length)
    {
        std::cerr << "error: " << length.error().message << '\n';
        return 1;
    }

    adbcpp::protocol::Message wrte;
    wrte.command = adbcpp::protocol::kWrte;
    wrte.arg0 = 7;
    wrte.arg1 = 2;
    wrte.data_length = *length;
    wrte.data_check = adbcpp::protocol::Message::compute_checksum(payload);
    wrte.magic = adbcpp::protocol::Message::compute_magic(wrte.command);
    transport.feed(wrte.encode());
    transport.feed(payload);

    adbcpp::protocol::Message clse;
    clse.command = adbcpp::protocol::kClse;
    clse.arg0 = 7;
    clse.arg1 = 2;
    clse.magic = adbcpp::protocol::Message::compute_magic(clse.command);
    transport.feed(clse.encode());

    auto connection = adbcpp::Connection::connect(transport);
    if (!connection)
    {
        std::cerr << "error: " << connection.error().message << '\n';
        return 1;
    }
    auto stream = adbcpp::Stream::open(*connection, "shell:echo hello");
    if (!stream)
    {
        std::cerr << "error: " << stream.error().message << '\n';
        return 1;
    }

    // read_all() acknowledges every WRTE and returns until the device closes.
    const auto output = stream->read_all();
    if (!output)
    {
        std::cerr << "error: " << output.error().message << '\n';
        return 1;
    }
    std::cout.write(reinterpret_cast<const char *>(output->data()),
                     static_cast<std::streamsize>(output->size()));
    return 0;
}
```

## Build a Message by Hand

`protocol::Message` is the 24-byte ADB header. `encode()` serializes it
little-endian; `compute_magic` and `compute_checksum` fill in the derived fields, and
`data_length_of` is the checked payload length. `compute_checksum` is the **sum of the
payload bytes**, matching AOSP's `calculate_apacket_checksum`, even though the protocol
document names the field `data_crc32` (blocker 27).

```cpp
#include <cstddef>
#include <iostream>
#include <span>
#include <string>

#include "adbcpp/adbcpp.hpp"

int main()
{
    const std::string banner = "host::features=shell_v2";
    const auto payload = std::span(reinterpret_cast<const std::byte *>(banner.data()), banner.size());

    const auto length = adbcpp::protocol::Message::data_length_of(payload);
    if (!length)
    {
        std::cerr << "error: " << length.error().message << '\n';
        return 1;
    }

    adbcpp::protocol::Message cnxn;
    cnxn.command = adbcpp::protocol::kCnxn;
    cnxn.arg0 = adbcpp::protocol::kVersion;
    cnxn.arg1 = adbcpp::protocol::kMaxData;
    cnxn.data_length = *length;
    cnxn.data_check = adbcpp::protocol::Message::compute_checksum(payload);
    cnxn.magic = adbcpp::protocol::Message::compute_magic(cnxn.command);

    const auto header = cnxn.encode();
    std::cout << "header is " << header.size() << " bytes\n";

    // decode() round-trips the header.
    const auto decoded = adbcpp::protocol::Message::decode(header);
    std::cout << "command: 0x" << std::hex << decoded.command << '\n';
    return 0;
}
```

## Use the Session Directly

`Session` frames a `Transport` into whole messages. It writes a header and its
payload as two separate transport writes, which is what USB needs.

```cpp
#include <cstddef>
#include <iostream>
#include <span>
#include <string>

#include "adbcpp/adbcpp.hpp"
#include "adbcpp/testing/mock_transport.hpp"

int main()
{
    adbcpp::testing::MockTransport transport;
    adbcpp::Session session(transport);

    const std::string text = "hello";
    const auto payload = std::span(reinterpret_cast<const std::byte *>(text.data()), text.size());
    const auto length = adbcpp::protocol::Message::data_length_of(payload);
    if (!length)
    {
        std::cerr << "error: " << length.error().message << '\n';
        return 1;
    }

    adbcpp::protocol::Message wrte;
    wrte.command = adbcpp::protocol::kWrte;
    wrte.arg0 = 2;
    wrte.arg1 = 7;
    wrte.data_length = *length;
    wrte.data_check = adbcpp::protocol::Message::compute_checksum(payload);
    wrte.magic = adbcpp::protocol::Message::compute_magic(wrte.command);

    const auto sent = session.send(wrte, payload);
    if (!sent)
    {
        std::cerr << "error: " << sent.error().message << '\n';
        return 1;
    }

    // The mock transport received the header and then the payload.
    std::cout << "wrote " << transport.written().size() << " bytes\n";
    return 0;
}
```

## Write a Custom Transport

Implement `Transport` to run the protocol over a different medium. Only `read`,
`write`, and `close` are needed; the protocol layers do not know what is
underneath.

```cpp
#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

#include "adbcpp/adbcpp.hpp"

class VectorTransport : public adbcpp::Transport
{
public:
    adbcpp::Result<std::size_t> read(std::span<std::byte> buffer) override
    {
        const std::size_t available = incoming_.size() - offset_;
        const std::size_t count = std::min(available, buffer.size());
        std::copy_n(incoming_.begin() + static_cast<std::ptrdiff_t>(offset_),
                    static_cast<std::ptrdiff_t>(count), buffer.begin());
        offset_ += count;
        return count; // 0 means end of stream.
    }

    adbcpp::Status write(std::span<const std::byte> data) override
    {
        outgoing_.insert(outgoing_.end(), data.begin(), data.end());
        return {};
    }

    void close() override { closed_ = true; }

    void feed(std::span<const std::byte> data)
    {
        incoming_.insert(incoming_.end(), data.begin(), data.end());
    }

    const std::vector<std::byte> &outgoing() const { return outgoing_; }
    bool closed() const { return closed_; }

private:
    std::vector<std::byte> incoming_;
    std::size_t offset_ = 0;
    std::vector<std::byte> outgoing_;
    bool closed_ = false;
};
```

## Detect the Authorization Fallback

On the very first connection, the device does not know the key yet and shows the
USB debugging prompt. `Connection::requested_authorization()` reports that the host
had to offer its public key, so an application can tell the user what is happening.

```cpp
#include <iostream>
#include <span>

#include "adbcpp/crypto/adb_key.hpp"
#include "adbcpp/usb/usb_transport.hpp"
#include "adbcpp/adbcpp.hpp"

int main()
{
    adbcpp::usb::DeviceId id;
    id.vendor_id = 0x22D9;
    id.product_id = 0x2769;

    auto transport = adbcpp::usb::UsbTransport::open(id);
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
    const auto public_key = std::span(reinterpret_cast<const std::byte *>(public_key_string.data()),
                                     public_key_string.size());

    auto connection = adbcpp::Connection::connect(*transport, public_key,
                                  [&key](std::span<const std::byte> token) { return key->sign(token); });
    if (!connection)
    {
        std::cerr << "error: " << connection.error().message << '\n';
        return 1;
    }

    if (connection->requested_authorization())
    {
        std::cerr << "approve the USB debugging prompt on the device\n";
    }

    const auto result = adbcpp::run(*connection, "id");
    if (!result)
    {
        std::cerr << "error: " << result.error().message << '\n';
        return 1;
    }
    std::cout << result->output;
    connection->close();
    return result->exit_code;
}
```

## Cheat Sheet

| I want to...                        | Use                                                     |
| ------------------------------------ | ------------------------------------------------------- |
| Check that a device is attached        | `adbcpp::usb::UsbTransport::is_present(id)` → `Result<bool>` |
| List attached devices with serials       | `adbcpp::usb::UsbTransport::list()` → `Result<std::vector<DeviceId>>` |
| Parse a `VID:PID` or `serial:` selector | `adbcpp::usb::DeviceId::parse(text)` → `Result<DeviceId>` |
| Open a USB transport                  | `adbcpp::usb::UsbTransport::open(id)` → `Result<UsbTransport>` |
| Open a TCP transport                  | `adbcpp::tcp::TcpTransport::open("localhost:5555")` → `Result<TcpTransport>` |
| Load or create the ADB key              | `adbcpp::crypto::Key::load_or_generate()` → `Result<Key>` |
| Sign an AUTH token                     | `key->sign(token)`                                      |
| Get the key's device fingerprint        | `key->fingerprint()` → `Result<std::string>`              |
| Handshake and connect                  | `adbcpp::Connection::connect(transport, public_key, signer)` → `Result<Connection>` |
| Connect with a retry, or reconnect        | `adbcpp::connect_with_retry(open, transport, public_key, signer)` → `Result<Connection>` |
| Close a connection                     | `connection->close()`                                   |
| Run a shell command                    | `adbcpp::run(connection, "echo hello")` → `Result<CommandResult>` |
| Force the v1 shell                     | `adbcpp::run(connection, cmd, adbcpp::ShellProtocol::V1)` |
| Read a command's output                 | `result->output`                                        |
| Read a command's stdout                  | `result->standard_output`                                 |
| Read a command's stderr                  | `result->error_output`                                    |
| Read a command's exit code              | `result->exit_code`                                     |
| Check whether a command worked           | `result->success`                                       |
| List a directory                        | `adbcpp::list(connection, "/sdcard")` → `Result<std::vector<DirEntry>>` |
| Check if an entry is a directory         | `entry.is_directory()`                                   |
| Check if an entry is a regular file       | `entry.is_regular()`                                     |
| Pull a file from the device              | `adbcpp::pull(connection, "/sdcard/a", "a")` → `Status`    |
| Push a file to the device                | `adbcpp::push(connection, "a", "/sdcard/a")` → `Status`    |
| Stat a path on the device                | `adbcpp::stat(connection, "/sdcard/a")` → `Result<std::optional<FileStat>>` |
| Install an APK                          | `adbcpp::install(connection, "app.apk", "-r")` → `Result<PackageResult>` |
| Uninstall a package                     | `adbcpp::uninstall(connection, "com.example.app")` → `Result<PackageResult>` |
| Read a package manager's failure reason    | `result->failure_reason()`                                |
| Launch an app                           | `adbcpp::launch(connection, "com.example.app")` → `Result<CommandResult>` |
| Close an app                            | `adbcpp::close(connection, "com.example.app")` → `Status`  |
| Check whether an app is running            | `adbcpp::is_running(connection, "com.example.app")` → `Result<bool>` |
| Open a service manually                | `adbcpp::Stream::open(connection, "shell:echo hello")` → `Result<Stream>` |
| Read a stream until the device closes    | `stream->read_all()` → `Result<std::vector<std::byte>>`    |
| Read an exact number of bytes            | `stream->read(buffer)` → `Status`                        |
| Write to a stream                      | `stream->write(bytes)` → `Status`                        |
| Wait for one transport to be readable     | `transport.wait_readable(timeout)` → `Result<bool>`         |
| Drive several devices from one thread      | `adbcpp::wait_readable(transports, timeout)` → `Result<std::optional<std::size_t>>` |
| Test without a device                  | `adbcpp::testing::MockTransport` + `feed()`              |
| Send/receive raw messages              | `adbcpp::Session`                                        |
| Inspect the negotiated features         | `connection->device_version()`, `connection->max_data()`     |
| Read the device's banner serial           | `connection->device_serial()`                              |
| Check a device feature                  | `connection->supports_feature("ls_v2")`                     |
| Check delayed acknowledgements           | `connection->supports_delayed_ack()`                       |
| Detect the authorization prompt          | `connection->requested_authorization()`                    |
| Enable logging                          | `adbcpp::set_logger(sink, level)`                        |
| Turn logging off                         | `adbcpp::clear_logger()`                                 |
| Check whether a level is logged           | `adbcpp::is_logging(level)` → `bool`                      |
| Write a message to the sink               | `adbcpp::log(level, message)`                             |

## Pitfalls

- **Nothing is thread-safe.** `Transport`, `Connection`, `Stream`, and `Key` each say so: they share mutable state, so concurrent use of one object must be serialized by the caller. One thread per device is the supported way to work with several devices at once, because every connection is independent. Two devices are needed rather than two connections to one, because the WinUSB driver admits a single handle per device, so the same device cannot be opened twice even from two processes (blocker 28). Two devices of the same model are told apart by `DeviceId::serial`, which `UsbTransport::list` fills in ([`03-roadmap.md`](03-roadmap.md#slice-8--select-a-device-by-serial)).
- **Keep the key alive.** The signer callback is stored by the `Connection`, so the
  `Key` it captures must outlive the connection. A dangling reference crashes on
  the first AUTH.
- **The logger is process-wide.** One sink is shared by every connection and thread,
  because the objects that log do not take one of their own. Install it before the
  connections are opened, and make the sink thread-safe if it shares state, because
  it may be called from several threads at once. `clear_logger()` turns logging off.
- **A USB wait costs a bulk transfer.** `UsbTransport::wait_readable` asks the endpoint
  with a bulk transfer bounded by the timeout, because libusb exposes no pollable handle
  on Windows and its poll-fd list is Linux and macOS only. Bytes that arrive are
  buffered for the next read, so nothing is lost. A transport that does not override
  `wait_readable` (a caller's own) reports not readable and is never returned by the
  helper.
- **Stop the `adb` server first.** `adb` claims the USB interface while it runs, so
  opening the same device fails with an access error. Stop the server before using
  `adbcpp`, and vice versa.
- **Replug after a failed run.** A failed transfer can leave the device's bulk
  endpoint halted; `clear_halt` recovers the host side, but the device may need a
  physical replug.
- **A transport read may be partial.** `Transport::read` may return fewer bytes than
  requested, and `Session` handles that; do not assume one read is one message.
- **`data_length` must match the payload.** `Session::send` takes the header and the
  payload separately, so a hand-built header whose `data_length` disagrees with the
  payload is an `InvalidArgument` and nothing is written, rather than a header that
  leaves the device waiting for bytes that never arrive (blocker 13).
- **`run` merges stdout and stderr in `output`.** They arrive interleaved, so
  `output`'s order is the device's, not stdout then stderr. `standard_output` and
  `error_output` carry each stream on its own under the `shell_v2` service; under the
  v1 `shell` service, which does not separate them, the two are empty.
- **This is not a full `adb` replacement yet.** The shell service, `sync`-based
  directory listing, file transfer in both directions, install and uninstall, app
  launch, close, and running checks, the USB and TCP transports, and selection by
  USB serial are exposed. There is no adb *server* protocol, so
  `host:connect`/`host:disconnect` are not
  ([`03-roadmap.md`](03-roadmap.md)).
- **`is_running` matches a process name.** `pidof` takes a process name, not a
  package name. A process is named after its package by default, so the two usually
  agree, but an application that renames its process makes `is_running` an
  approximation.
- **`close` cannot tell whether the package existed.** `am force-stop` exits zero
  even for a package that is not installed and prints nothing, so `close` returns a
  `Status` and a caller cannot tell "stopped" from "was not installed".
- **`value()` and `error()` assert on the wrong alternative.** Check a `Result` with
  `has_value()` or `operator bool` first, then unwrap it with `operator*` or
  `operator->`. The library never calls `value()` or `error()` without checking, and
  neither should a caller: calling either on the wrong alternative is a programming
  error, not a failure the library reports.
- **A package manager rejection is not an error.** `install` and `uninstall` return
  `success == false` with the device's output rather than an `Error`, because a rejected
  APK and a package that cannot be removed are normal answers. An `Error` means the
  transfer or the stream failed instead.
- **A pushed APK is left in `/data/local/tmp` if the process dies.** `install`
  removes it after the install, and it also removes it when the install is rejected,
  but a process that is killed part way through leaves it behind.
- **An APK name or package name is single-quoted.** The device runs the command
  through `sh -c`, so a name with a space or a quote would otherwise be split into
  several words. `install` and `uninstall` quote it, but `options` are passed through
  as they are and so must not come from untrusted input.
- **A pulled file may be partial.** `pull` creates or truncates the local file before
  the transfer starts, so a transfer that fails leaves the chunks received so far
  behind. The caller decides whether to retry or remove it.
- **A pushed file may have wider permissions.** The device copies the user permission
  bits to the group and other bits, so a `0644` local file becomes `0666` on the
  device. That is the daemon's behaviour, not the library's.
- **A very large file on a slow device may need a longer budget.** `UsbTransport`
  bounds each bulk transfer with a short timeout and retries it while nothing was
  transferred, so a device that has gone quiet is noticed quickly. The total wait is the
  budget, which defaults to two minutes and covers the human-paced approval of the
  on-device debugging prompt. Raise it with `set_transfer_budget`, or `--budget <ms>` on
  the example, when a device is slower than that.
- **A stream ignores frames for other streams.** Frames carry the recipient's local id
  in `arg1`, and the device can send a `CLOSE` for a previous stream while the next
  one opens, so `Stream` skips any frame whose `arg1` is not its own local id. Do not
  assume every frame on the connection belongs to the stream you just opened
  ([`06-sync-protocol.md`](06-sync-protocol.md), blocker 18).
