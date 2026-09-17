# Learning Path

A guided curriculum for using `adbcpp` as a starting point to learn the ADB
protocol and how to build a protocol client from scratch. It is written for a
developer who can read C++20 and wants to understand **why** each layer exists,
not only what it does.

The repository is intentionally small: every layer is a thin slice that ends in
something runnable. Read the code in the order below and each piece will have a
reason to exist by the time you reach it.

## How to Use This Curriculum

Every module has the same shape:

- **Goal** — what you should understand when you are done.
- **Read** — the sources and documents to read, in order.
- **Run** — the tests or examples that demonstrate it.
- **Exercise** — a small change to make yourself to prove you understood it.
- **Checkpoint** — questions you should be able to answer without looking.

Do the modules in order. Modules 1–4 build the mental model; modules 5–9
apply it. If you get stuck, [`docs/04-blockers.md`](docs/04-blockers.md) is the
"answer key": it records the non-obvious problems and how they were solved.

## Prerequisites

- **C++20**, **CMake 3.24+**, and **Git**. See the [README](README.md#building)
  for platform-specific setup.
- Optional but strongly recommended: an **Android device** with USB debugging, plus
  **USBPcap + Wireshark** to capture a real `adb` session for comparison.

Build once and keep the tests running as you go:

```
cmake -S . -B build -DADBCPP_BUILD_TESTS=ON -DADBCPP_BUILD_EXAMPLES=ON
cmake --build build --config Release
ctest --test-dir build --output-on-failure -C Release -E device
```

The `-E device` flag skips the test that needs a real device. Drop it when one
is attached.

## Module 0 — Orientation

**Goal.** Understand what ADB is, what problem `adbcpp` solves, and which of
ADB's two protocols it implements.

**Read.**

1. [`README.md`](README.md) — the project in one screen.
2. [`docs/01-objective.md`](docs/01-objective.md) — the constraint: no `adb`
   server, no `adb` binary.
3. [`docs/02-references.md`](docs/02-references.md) — where to read about the
   protocol.
4. [`docs/03-roadmap.md`](docs/03-roadmap.md) — the vertical-slice plan you
   will be following.

The key idea: `adb` is really two protocols. The command line talks to a long-lived
**server** on port 5037, and that server talks to `adbd` on the device. This
library skips the server and speaks the second protocol directly. Everything after
this module is that second protocol.

**Run.**

```
build/examples/Release/adbcpp_sample        # or the single-config path
ctest --test-dir build -C Release -E device
```

**Exercise.** Open [`examples/sample/main.cpp`](examples/sample/main.cpp), change
the mock device's `arg1` (the maximum payload) from `4096` to `0x100000`, rebuild,
and confirm the printed value changes. Notice that a message is a header plus a
payload, and that the sample only feeds the header because the payload is empty.

**Checkpoint.**

- Why does `adbcpp` not need a server on port 5037?
- What are the two roles of `arg0` and `arg1` in a CNXN message?

## Module 1 — Byte Channels and Framing

**Goal.** Understand the split between moving bytes (`Transport`) and moving
messages (`Session`).

**Read.**

1. [`include/adbcpp/transport.hpp`](include/adbcpp/transport.hpp) — the only
   abstraction over the medium.
2. [`include/adbcpp/session.hpp`](include/adbcpp/session.hpp) and
   [`src/session.cpp`](src/session.cpp) — framing on top of a byte channel.
3. [`include/adbcpp/protocol/message.hpp`](include/adbcpp/protocol/message.hpp)
   and [`src/protocol/message.cpp`](src/protocol/message.cpp) — the header.
4. [`include/adbcpp/testing/mock_transport.hpp`](include/adbcpp/testing/mock_transport.hpp)
   — the in-memory transport used by every test.

**Run.**

```
ctest --test-dir build -C Release -R "session|protocol" --output-on-failure
```

**Exercise.** Add a test to [`tests/session_test.cpp`](tests/session_test.cpp)
that feeds a WRTE header in two fragments and its payload in two more, and assert
the frame is still reassembled. `read_exact` is what makes this work.

**Checkpoint.**

- Why is a header written as its own transport write instead of being concatenated
  with the payload?
- What does `Transport::read` return when the stream ends, and how does `Session`
  react?

## Module 2 — The Message Header on the Wire

**Goal.** Be able to decode the 24-byte header by hand.

**Read.**

1. [`include/adbcpp/protocol/message.hpp`](include/adbcpp/protocol/message.hpp) —
   the six fields and their meaning.
2. [`include/adbcpp/protocol/commands.hpp`](include/adbcpp/protocol/commands.hpp)
   — the command constants and `make_command`.
3. [`src/protocol/message.cpp`](src/protocol/message.cpp) — the little-endian
   serialization and the CRC-32.
4. AOSP's protocol document, "protocol overview and basics":
   <https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/docs/dev/protocol.md>

**Exercise.** Take the bytes `43 4e 58 4e` and decode them as a command. Then
work out why `magic` is `command ^ 0xFFFFFFFF`. Finally, compute the CRC-32 of
`"123456789"` and check it against `0xCBF43926`, the standard test vector.

**Checkpoint.**

- What happens, per the protocol document, if a receiver sees a bad header, a bad
  payload, or an unknown command?
- Why is the CRC-32 still computed even though `adbd` no longer verifies it?

## Module 3 — CNXN and the Device Banner

**Goal.** Understand the first half of the handshake and why the banner matters.

**Read.**

1. [`include/adbcpp/connection.hpp`](include/adbcpp/connection.hpp) — `kSystemIdentity`.
2. [`src/connection.cpp`](src/connection.cpp) — the CNXN exchange.
3. [`include/adbcpp/protocol/commands.hpp`](include/adbcpp/protocol/commands.hpp)
   — `kVersion` and `kMaxData`.

The host banner is not decorative. `adbd` **resets its own feature set** from the
host's `features=` list, so an empty list makes the device treat the host as
supporting nothing (blocker 6). This is the first place where "looks fine" is not
enough.

**Run.** With a device attached:

```
build/examples/Release/adbcpp_usb_example "echo hello"
```

**Exercise.** Capture a real `adb` session and one `adbcpp` session with USBPcap +
Wireshark (see [Debugging Toolkit](#debugging-toolkit)) and compare the CNXN
headers and banners byte-for-byte. They should match.

**Checkpoint.**

- Why must `arg1` be at least large enough for a 256-byte signature?
- Why does `adbcpp` send its CNXN before it knows the device's `maxdata`?

## Module 4 — AUTH, Keys, and Signing

**Goal.** Understand how authentication works and why the signature is subtle.

**Read.**

1. [`include/adbcpp/crypto/adb_key.hpp`](include/adbcpp/crypto/adb_key.hpp) and
   [`src/crypto/adb_key.cpp`](src/crypto/adb_key.cpp) — the key formats.
2. [`src/connection.cpp`](src/connection.cpp) — the AUTH exchange and fallback.
3. Blockers 8, 9, 10, 11, and 16 in
   [`docs/04-blockers.md`](docs/04-blockers.md).

This module contains the project's hardest bug. The device sends a 20-byte token;
the host signs it with `RSA_sign(NID_sha1, token, ...)`. The token **is** the
SHA-1 digest, so it must not be hashed again. Hashing it first produces a
perfectly valid-looking signature that the device silently rejects, which is why the
bug survived so long (blocker 16).

**Exercise.** With the device attached, use the `key fingerprint` printed by the
example and check it against the device's authorized-computers list. Then read the
AUTH frames from a capture and verify the signature over the token directly, and
show that verifying over `SHA1(token)` fails.

**Checkpoint.**

- Why does `adb` use the same `~/.android/adbkey` files?
- What is different about the ADB public key format compared with a DER
  `SubjectPublicKeyInfo`?
- When does the host send AUTH type 3 instead of type 2?

## Module 5 — The USB Transport

**Goal.** Understand how a byte channel is realized over USB.

**Read.**

1. [`include/adbcpp/usb/usb_transport.hpp`](include/adbcpp/usb/usb_transport.hpp)
   — the ADB interface class/subclass/protocol.
2. [`src/usb/usb_transport.cpp`](src/usb/usb_transport.cpp) — enumeration,
   claiming, and the bulk transfers.
3. Blockers 3, 4, and 5 in [`docs/04-blockers.md`](docs/04-blockers.md).

The ADB function is a vendor-specific USB interface (`0xFF`/`0x42`/`0x01`) with
two bulk endpoints. The crucial difference from TCP is that a bulk transfer is a
**discrete message**: a header and its payload arrive as two separate transfers,
which is why `Session` writes them separately (blocker 3).

**Exercise.** In [`examples/usb/main.cpp`](examples/usb/main.cpp), print the
interface number and both endpoint addresses after opening the transport, and compare
them with what a capture shows for `adb`.

**Checkpoint.**

- Why does `UsbTransport::read` return at most one transfer's worth of bytes?
- What does `libusb_clear_halt` fix, and what can it not fix?

## Module 6 — Streams and Services

**Goal.** Understand `OPEN`/`OKAY`/`WRTE`/`CLSE` and the local/remote id model.

**Read.**

1. [`include/adbcpp/stream.hpp`](include/adbcpp/stream.hpp) and
   [`src/stream.cpp`](src/stream.cpp) — the stream lifecycle.
2. [`include/adbcpp/shell.hpp`](include/adbcpp/shell.hpp) and
   [`src/shell.cpp`](src/shell.cpp) — a service on top of a stream.
3. AOSP's protocol document, the `OPEN`, `READY`, `WRITE`, and `CLOSE` sections.
4. Blocker 13 in [`docs/04-blockers.md`](docs/04-blockers.md) — the `OPEN`
   payload bug.

A stream is a small state machine keyed by two ids, and the ids are **relative to
the sender**, so each side's `local-id` is the other's `remote-id`. The `OPEN`
payload is a null-terminated service name, unlike the CNXN banner.

**Exercise.** Add a `read_line` helper to `Stream` that reads one byte at a time
until `'\n'`, then use it to read a shell command's output line by line. Notice
that it has to keep reading across `WRTE` boundaries.

**Checkpoint.**

- Which id must never be zero, and which may be zero only for a failed `OPEN`?
- Why is the `OPEN` payload null-terminated when the CNXN banner is not?

## Module 7 — The sync Service

**Goal.** Understand how a service with its own binary protocol is built on top of
`Stream`, and why a second framing layer is needed.

**Read.**

1. [`docs/06-sync-protocol.md`](docs/06-sync-protocol.md) — the wire format,
   field by field.
2. [`include/adbcpp/sync.hpp`](include/adbcpp/sync.hpp) and
   [`src/sync.cpp`](src/sync.cpp) — `list` and `DirEntry`.
3. AOSP's `docs/dev/sync.md`:
   <https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/docs/dev/sync.md>
4. Blockers 17 and 18 in [`docs/04-blockers.md`](docs/04-blockers.md).

`sync` is not a new transport: it is a second binary protocol inside the payload
of a `WRTE`. Its requests are an id and a length, and the path is **not**
null-terminated, unlike an `OPEN` payload. The v2 form (`LIS2`/`DNT2`) is chosen
when the device advertises `ls_v2`, and it carries a 64-bit size and a per-entry
error.

**Run.**

```
build/examples/Release/adbcpp_sync_example
ctest --test-dir build -C Release -R "sync|example_sync" --output-on-failure
```

**Exercise.** Extend `DirEntry` with `uid` and `gid` from the v2 body and print
them in the example. Then make `list` always use the v1 form and observe which
fields go missing and why.

**Checkpoint.**

- Why is the path not null-terminated when an `OPEN` payload is?
- Why must `DONE`'s body still be read even though it carries no data?
- Why does the device answer a sync request with an `OKAY`, and why did the shell
  service never reveal that?

## Module 8 — shell_v2

**Goal.** Understand how a shell command becomes structured output.

**Read.**

1. [`include/adbcpp/shell.hpp`](include/adbcpp/shell.hpp) and
   [`src/shell.cpp`](src/shell.cpp) — the packet framing.
2. AOSP's `shell_protocol.h`:
   <https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/shell_protocol.h>
3. Blocker 14 in [`docs/04-blockers.md`](docs/04-blockers.md).

`shell_v2` packets are **not** ADB commands. They are the payload of `WRTE`
messages, and each is a 1-byte id followed by a 4-byte little-endian length. For
the exit packet the length field carries the exit status itself.

**Exercise.** Extend `CommandResult` to keep stdout and stderr separate, and update
[`tests/device_test.cpp`](tests/device_test.cpp) to check both. Note that the
device interleaves them, so order is not guaranteed.

**Checkpoint.**

- Why does `shell:` give you no exit code, while `shell,v2` does?
- What does the `raw` in `shell,v2,raw:` change?

## Module 9 — Flow Control and Robustness

**Goal.** Understand the failure modes that are invisible on a healthy device.

**Read.**

1. `kInitialDelayedAckBytes` in
   [`include/adbcpp/protocol/commands.hpp`](include/adbcpp/protocol/commands.hpp).
2. The window handling in [`src/stream.cpp`](src/stream.cpp) and
   [`src/connection.cpp`](src/connection.cpp).
3. Blockers 4, 12, and 15 in [`docs/04-blockers.md`](docs/04-blockers.md).
4. AOSP's `docs/dev/delayed_ack.md`.

Delayed acknowledgements change the meaning of `OPEN.arg1` from "unused" to the
initial "available send bytes" window. Claiming the feature without implementing
it makes the device close the stream (blocker 12), so `adbcpp` leaves it off.

**Exercise.** Re-enable `advertise_delayed_ack` in the USB example, run against a
real device, and observe the failure. Then explain why the default is off.

**Checkpoint.**

- Why must a `WRTE` not be sent before the stream is ready?
- What does a stalled bulk endpoint look like from the host, and how is it cleared?

## Module 10 — Where to Go Next

**Goal.** Choose the next slice and apply everything you have learned.

**Read.** [`docs/03-roadmap.md`](docs/03-roadmap.md) — the remaining slices:
pull, push, install/uninstall, app control, and finally TCP plus silent
authentication.

**Exercise.** Implement Slice 3 (pull a file with the sync `RECV` request, whose
chunks are `DATA` messages followed by `DONE`). The `sync` service is documented
in AOSP's `docs/dev/sync.md`:
<https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/docs/dev/sync.md>

Note how much of the work is already done: `Stream::read` reads an exact byte
count across `WRTE` messages, which is exactly what the chunked `RECV` response
needs. Only the request/response framing is new.

**Checkpoint.**

- Which layer does a new service belong in, and which layers does it not touch?
- Which existing tests would fail if you broke the framing layer?

## Debugging Toolkit

When something does not work, capture a real `adb` session and diff it against
yours. This is how blockers 12 and 16 were solved.

1. Install Wireshark with the **USBPcap** component and reboot.
2. Find the USBPcap interface for the root hub the device is on:
   ```
   & "C:\Program Files\Wireshark\tshark.exe" -D
   ```
   Verify by capturing a few seconds on each and keeping the one that shows
   `usb.idVendor == 0x22d9`.
3. Capture while running `adb` (the baseline), then while running `adbcpp`:
   ```
   & "C:\Program Files\Wireshark\tshark.exe" -i <index> -a duration:12 -w adb.pcapng -q
   ```
4. Extract the `CNXN` and `AUTH` frames and compare:
   ```
   & "C:\Program Files\Wireshark\tshark.exe" -r adb.pcapng `
       -Y "usb.capdata contains 43:4e:58:4e || usb.capdata contains 41:55:54:48" `
       -T fields -e frame.number -e usb.endpoint_address -e usb.capdata
   ```
5. The device sends a **fresh random token per connection**, so signatures
   cannot be compared across runs. Compare them against the **same token**, and
   verify them with `openssl` or the .NET `RSA` class.

A few practical notes:

- A failed run can leave the device's bulk endpoint stalled until the device is
  replugged (blocker 4).
- `adb` holds the USB interface while it runs, so stop the `adb` server before
  running `adbcpp`, and vice versa (blocker 5).
- The first connection after idle can time out; re-running usually succeeds
  (blocker 15).

## Learning Outcomes

When you have finished, you should be able to:

- Explain ADB's two protocols and why `adbcpp` implements only the direct one.
- Decode a 24-byte ADB header by hand, including `magic` and the CRC-32.
- Describe the CNXN/AUTH handshake, including when the public key is sent.
- Explain why the AUTH token is signed as the SHA-1 digest and not re-hashed.
- Describe how a byte channel becomes messages and streams over USB.
- Implement a new service on top of `Stream` without touching the lower layers.
- List a directory over the `sync` service and explain how its binary framing
  differs from the ADB protocol.
- Capture a real `adb` session and use it as a baseline to debug your own.
