# Usage

Practical, copy-pasteable examples for everything `adbcpp` can do today: connect
over USB, run a shell command, list a directory, pull and push files, stat a path,
work with the ADB key, and use the lower-level protocol layers directly. Every
example compiles against the library as it stands now. For the theory behind them, read
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
| `adbcpp::adbcpp` | Protocol, session, streams, shell, mock transport | none             |
| `adbcpp::crypto` | `adbcpp::crypto::Key` (ADB key pair)           | mbedTLS          |
| `adbcpp::usb`    | `adbcpp::usb::UsbTransport` (USB transport)    | libusb           |

`adbcpp::crypto` and `adbcpp::usb` both link `adbcpp::adbcpp` publicly, so
linking the USB backend pulls in the rest. `adbcpp::usb` is only defined when the
project is built with `ADBCPP_BUILD_USB=ON` (the default at the top level).

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
#include "adbcpp/adbcpp.hpp"                 // core: Connection, Stream, run, protocol
#include "adbcpp/sync.hpp"                    // adbcpp::list, adbcpp::DirEntry
#include "adbcpp/crypto/adb_key.hpp"          // adbcpp::crypto::Key
#include "adbcpp/usb/usb_transport.hpp"       // adbcpp::usb::UsbTransport
#include "adbcpp/testing/mock_transport.hpp"   // adbcpp::testing::MockTransport
```

## Run a Shell Command over USB

This is the main use case: connect directly to a device over USB and run a command,
without an `adb` server or the `adb` binary.

```cpp
#include <iostream>
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

    // Opening a device that is absent (or claimed by a running adb server) throws,
    // so check for it first.
    if (!adbcpp::usb::UsbTransport::is_present(id))
    {
        std::cerr << "no matching USB device found\n";
        return 1;
    }

    adbcpp::usb::UsbTransport transport(id);

    // Reuse adb's key, so the device does not show the approval prompt. The key
    // must outlive the connection, because the signer below captures it.
    const auto key = adbcpp::crypto::Key::load_or_generate();
    const std::string &public_key_string = key.public_key();
    const auto public_key = std::span(reinterpret_cast<const std::byte *>(public_key_string.data()),
                                     public_key_string.size());

    // The connection performs the CNXN/AUTH handshake on construction.
    adbcpp::Connection connection(transport, public_key,
                                  [&key](std::span<const std::byte> token) { return key.sign(token); });

    const adbcpp::CommandResult result = adbcpp::run(connection, "echo hello");

    std::cout << "exit code: " << static_cast<int>(result.exit_code) << '\n';
    std::cout << result.output;

    transport.close();
    return result.exit_code;
}
```

`CommandResult::output` is stdout and stderr combined, in the order the device
produced them, and `exit_code` is the command's status.

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

    adbcpp::usb::UsbTransport transport(id);
    const auto key = adbcpp::crypto::Key::load_or_generate();
    const std::string &public_key_string = key.public_key();
    const auto public_key = std::span(reinterpret_cast<const std::byte *>(public_key_string.data()),
                                     public_key_string.size());

    adbcpp::Connection connection(transport, public_key,
                                  [&key](std::span<const std::byte> token) { return key.sign(token); });

    for (const auto &entry : adbcpp::list(connection, "/sdcard"))
    {
        std::cout << (entry.is_directory() ? 'd' : '-') << ' ' << entry.size << ' ' << entry.name << '\n';
    }

    transport.close();
    return 0;
}
```

`DirEntry` carries the POSIX metadata the device reports, the same values
`lstat` would give: `name`, `mode`, `size`, and `mtime`, plus `is_directory()`
and `is_regular()`. A file larger than 4 GiB is reported correctly, because the
v2 entry form uses a 64-bit size.

### List a Directory Without a Device

To run `list` and `pull` with no device attached, see
[`examples/sync/main.cpp`](../examples/sync/main.cpp). It queues a device's CNXN and
its `DNT2`, `DATA`, and `DONE` responses on the mock transport, calls the real
`list` and `pull`, prints the entries and the pulled contents, and then prints the
`LIS2` and `RECV` request headers that they wrote. It is registered with CTest as
`example_sync` and runs in CI. The wire format itself is explained in
[`06-sync-protocol.md`](06-sync-protocol.md).

## Pull a File

`pull` copies a file from the device to a local path. It writes each `DATA` chunk as
it arrives, so the file is never held in memory whole and a large file costs no more
memory than a small one.

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

    adbcpp::usb::UsbTransport transport(id);
    const auto key = adbcpp::crypto::Key::load_or_generate();
    const std::string &public_key_string = key.public_key();
    const auto public_key = std::span(reinterpret_cast<const std::byte *>(public_key_string.data()),
                                     public_key_string.size());

    adbcpp::Connection connection(transport, public_key,
                                  [&key](std::span<const std::byte> token) { return key.sign(token); });

    // The local file is created or truncated, and its directory must exist.
    adbcpp::pull(connection, "/sdcard/Download/report.pdf", "report.pdf");

    transport.close();
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

    adbcpp::usb::UsbTransport transport(id);
    const auto key = adbcpp::crypto::Key::load_or_generate();
    const std::string &public_key_string = key.public_key();
    const auto public_key = std::span(reinterpret_cast<const std::byte *>(public_key_string.data()),
                                     public_key_string.size());

    adbcpp::Connection connection(transport, public_key,
                                  [&key](std::span<const std::byte> token) { return key.sign(token); });

    // An existing directory receives the file under its local name.
    adbcpp::push(connection, "report.pdf", "/sdcard/Download/");

    transport.close();
    return 0;
}
```

The device creates the destination, or overwrites it if it already exists, with the
local file's permissions and modification time.

## Stat a Path

`stat` reports a path's metadata, following symbolic links. A path that does not
exist is returned as `std::nullopt` rather than thrown.

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

    adbcpp::usb::UsbTransport transport(id);
    const auto key = adbcpp::crypto::Key::load_or_generate();
    const std::string &public_key_string = key.public_key();
    const auto public_key = std::span(reinterpret_cast<const std::byte *>(public_key_string.data()),
                                     public_key_string.size());

    adbcpp::Connection connection(transport, public_key,
                                  [&key](std::span<const std::byte> token) { return key.sign(token); });

    const auto info = adbcpp::stat(connection, "/sdcard/Download/report.pdf");
    if (info)
    {
        std::cout << (info->is_directory() ? 'd' : '-') << ' ' << info->size << '\n';
    }
    else
    {
        std::cout << "no such path\n";
    }

    transport.close();
    return 0;
}
```

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

    std::cout << "fingerprint: " << key.fingerprint() << '\n';
    std::cout << "public key:  " << key.public_key() << '\n';
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
    adbcpp::Connection connection(transport);

    std::cout << "device version: 0x" << std::hex << connection.device_version() << '\n';
    std::cout << "device max data: " << std::dec << connection.max_data() << '\n';
    return 0;
}
```

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

    adbcpp::protocol::Message wrte;
    wrte.command = adbcpp::protocol::kWrte;
    wrte.arg0 = 7;
    wrte.arg1 = 2;
    wrte.data_length = adbcpp::protocol::Message::data_length_of(payload);
    wrte.data_crc32 = adbcpp::protocol::Message::compute_crc32(payload);
    wrte.magic = adbcpp::protocol::Message::compute_magic(wrte.command);
    transport.feed(wrte.encode());
    transport.feed(payload);

    adbcpp::protocol::Message clse;
    clse.command = adbcpp::protocol::kClse;
    clse.arg0 = 7;
    clse.arg1 = 2;
    clse.magic = adbcpp::protocol::Message::compute_magic(clse.command);
    transport.feed(clse.encode());

    adbcpp::Connection connection(transport);
    adbcpp::Stream stream(connection, "shell:echo hello");

    // read_all() acknowledges every WRTE and returns until the device closes.
    const auto output = stream.read_all();
    std::cout.write(reinterpret_cast<const char *>(output.data()),
                     static_cast<std::streamsize>(output.size()));
    return 0;
}
```

## Build a Message by Hand

`protocol::Message` is the 24-byte ADB header. `encode()` serializes it
little-endian; `compute_magic` and `compute_crc32` fill in the derived fields, and
`data_length_of` is the checked payload length.

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

    adbcpp::protocol::Message cnxn;
    cnxn.command = adbcpp::protocol::kCnxn;
    cnxn.arg0 = adbcpp::protocol::kVersion;
    cnxn.arg1 = adbcpp::protocol::kMaxData;
    cnxn.data_length = adbcpp::protocol::Message::data_length_of(payload);
    cnxn.data_crc32 = adbcpp::protocol::Message::compute_crc32(payload);
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

    adbcpp::protocol::Message wrte;
    wrte.command = adbcpp::protocol::kWrte;
    wrte.arg0 = 2;
    wrte.arg1 = 7;
    wrte.data_length = adbcpp::protocol::Message::data_length_of(payload);
    wrte.data_crc32 = adbcpp::protocol::Message::compute_crc32(payload);
    wrte.magic = adbcpp::protocol::Message::compute_magic(wrte.command);

    session.send(wrte, payload);

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
    std::size_t read(std::span<std::byte> buffer) override
    {
        const std::size_t available = incoming_.size() - offset_;
        const std::size_t count = std::min(available, buffer.size());
        std::copy_n(incoming_.begin() + static_cast<std::ptrdiff_t>(offset_),
                    static_cast<std::ptrdiff_t>(count), buffer.begin());
        offset_ += count;
        return count; // 0 means end of stream.
    }

    void write(std::span<const std::byte> data) override
    {
        outgoing_.insert(outgoing_.end(), data.begin(), data.end());
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

    adbcpp::usb::UsbTransport transport(id);
    const auto key = adbcpp::crypto::Key::load_or_generate();
    const std::string &public_key_string = key.public_key();
    const auto public_key = std::span(reinterpret_cast<const std::byte *>(public_key_string.data()),
                                     public_key_string.size());

    adbcpp::Connection connection(transport, public_key,
                                  [&key](std::span<const std::byte> token) { return key.sign(token); });

    if (connection.requested_authorization())
    {
        std::cerr << "approve the USB debugging prompt on the device\n";
    }

    const auto result = adbcpp::run(connection, "id");
    std::cout << result.output;
    transport.close();
    return result.exit_code;
}
```

## Cheat Sheet

| I want to...                        | Use                                                     |
| ------------------------------------ | ------------------------------------------------------- |
| Check that a device is attached        | `adbcpp::usb::UsbTransport::is_present(id)`             |
| Open a USB transport                  | `adbcpp::usb::UsbTransport transport(id)`               |
| Load or create the ADB key              | `adbcpp::crypto::Key::load_or_generate()`               |
| Sign an AUTH token                     | `key.sign(token)`                                       |
| Get the key's device fingerprint        | `key.fingerprint()`                                     |
| Handshake and connect                  | `adbcpp::Connection connection(transport, public_key, signer)` |
| Run a shell command                    | `adbcpp::run(connection, "echo hello")`                  |
| Read a command's output                 | `result.output`                                         |
| Read a command's exit code              | `result.exit_code`                                      |
| List a directory                        | `adbcpp::list(connection, "/sdcard")`                    |
| Check if an entry is a directory         | `entry.is_directory()`                                   |
| Check if an entry is a regular file       | `entry.is_regular()`                                     |
| Pull a file from the device              | `adbcpp::pull(connection, "/sdcard/a", "a")`              |
| Push a file to the device                | `adbcpp::push(connection, "a", "/sdcard/a")`              |
| Stat a path on the device                | `adbcpp::stat(connection, "/sdcard/a")`                   |
| Open a service manually                | `adbcpp::Stream stream(connection, "shell:echo hello")`   |
| Read a stream until the device closes    | `stream.read_all()`                                     |
| Read an exact number of bytes            | `stream.read(buffer)`                                    |
| Write to a stream                      | `stream.write(bytes)`                                   |
| Test without a device                  | `adbcpp::testing::MockTransport` + `feed()`              |
| Send/receive raw messages              | `adbcpp::Session`                                        |
| Inspect the negotiated features         | `connection.device_version()`, `connection.max_data()`     |
| Check a device feature                  | `connection.supports_feature("ls_v2")`                     |
| Check delayed acknowledgements           | `connection.supports_delayed_ack()`                       |
| Detect the authorization prompt          | `connection.requested_authorization()`                    |

## Pitfalls

- **Keep the key alive.** The signer callback is stored by the `Connection`, so the
  `Key` it captures must outlive the connection. A dangling reference crashes on
  the first AUTH.
- **Stop the `adb` server first.** `adb` claims the USB interface while it runs, so
  opening the same device fails with an access error. Stop the server before using
  `adbcpp`, and vice versa.
- **Replug after a failed run.** A failed transfer can leave the device's bulk
  endpoint halted; `clear_halt` recovers the host side, but the device may need a
  physical replug.
- **A transport read may be partial.** `Transport::read` may return fewer bytes than
  requested, and `Session` handles that; do not assume one read is one message.
- **`run` merges stdout and stderr.** They arrive interleaved, so the order is not
  guaranteed. `CommandResult` does not separate them.
- **This is not a full `adb` replacement yet.** The shell service, `sync`-based
  directory listing, and file transfer in both directions are exposed; install and app
  control are still on the roadmap ([`03-roadmap.md`](03-roadmap.md)).
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
