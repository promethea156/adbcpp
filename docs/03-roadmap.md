# Roadmap

This document outlines the planned implementation of `adbcpp` as a series of **vertical slices**. Each slice delivers a thin, end-to-end capability that spans every layer it needs (protocol, transport, session, service, public API), and ships with its own tests, documentation, and example. Layers are fleshed out incrementally, only as far as each slice requires.

This is deliberate: rather than completing an entire layer before anything is runnable, every slice ends with something demonstrable against a real device (except Slice 0).

## Current Status

**Slice 0 — complete.** CMake project, Catch2 tests, sample, Doxygen, CI.

**Slice 1 — complete.** `echo hello` runs over USB against a real device and its output is captured:

- USB transport, the `CNXN` → `AUTH` → signed reply → device `CNXN` handshake, the `adbkey`/`adbkey.pub` key pair, and the `OPEN`/`OKAY`/`WRTE`/`CLSE` stream lifecycle with `shell_v2` packet parsing.
- Public API: `Connection`, `Stream`, and `run(connection, command)`, which returns a `Result<CommandResult>` holding the merged output, the exit code, and whether the command succeeded.

**Slice 2 — complete.** A directory is listed over the `sync` service and its entries are returned as structured data:

- `sync:` stream setup, which puts the stream in a binary mode that differs from the ADB protocol, and `LIST`/`DENT` parsing using the v2 `LIS2`/`DNT2` form when the device advertises `ls_v2`.
- `Stream::read` reads an exact byte count across `WRTE` messages, which the length-prefixed sync responses need.
- Public API: `list(connection, path)` returning `DirEntry` values with `name`, `mode`, `size`, and `mtime`.

The wire format is documented in [`06-sync-protocol.md`](06-sync-protocol.md), and `examples/sync/main.cpp` runs the whole exchange against the mock transport, so `list` is exercised with no device attached.

**Slice 3 — complete.** A file is copied from the device with the `sync` `RECV` request:

- `RECV` and the `DATA` chunks that follow, ending with `DONE`. Each chunk is written to the local file as it arrives, so a file of any size costs no more memory than a small one.
- Public API: `pull(connection, remote, local)`.

**Slice 4 — complete.** A file is copied to the device with the `sync` `STAT` and `SEND` requests:

- `STAT` tells whether the destination is a directory, so an existing directory receives the file under its local name.
- `SEND`'s path is a `<path>,<mode>` spec, and its final `DONE`, whose size is the file's modification time, is the one request the device answers with `OKAY`.
- Public API: `stat(connection, path)`, which follows symbolic links and returns a missing path as an empty result, and `push(connection, local, remote)`.

**Slice 5 — complete.** An APK is installed and a package is uninstalled:

- `install` pushes the APK into the device's `/data/local/tmp` with `push`, runs `pm install` over the shell service, and removes the pushed copy whether the install worked or not. `uninstall` runs `pm uninstall`, and `keep_data` adds `-k`.
- `options` are passed to `pm install` verbatim, so `-r` reinstalls a package in place and keeps its data.
- Public API: `install(connection, apk, options)` and `uninstall(connection, package, keep_data)`, both returning a `Result<PackageResult>`.

Nothing was needed at the protocol layer: the slice is composition, as the roadmap predicted. `push` is the `sync` `SEND` request and `pm install` is a shell command, so both already existed.

Every slice so far hid at least one non-obvious problem. The record of each — how it showed itself, what caused it, and how it was solved — is in [`04-blockers.md`](04-blockers.md) rather than repeated here.

**Slice 6 — complete.** An app is launched, detected, and closed:

- `launch` runs `am start -W <package>`. A bare positional argument is the form `am start` resolves as `ACTION_MAIN` with `CATEGORY_LAUNCHER` and the package, so the device starts the package's launcher activity; `-W` waits for the launch, so a following `is_running` is meaningful.
- `close` runs `am force-stop <package>`, and `is_running` runs `pidof <package>`, whose exit code answers both cases: zero with the pids when the process runs, nonzero with no output when it does not.
- Public API: `launch(connection, package)` returning a `Result<CommandResult>`, `close(connection, package)` returning a `Status`, and `is_running(connection, package)` returning a `Result<bool>`.

Nothing was needed at the protocol layer: the slice is composition again, as the roadmap predicted. `am start`, `am force-stop`, and `pidof` are shell commands, so all three already existed.

**Slice 7 — complete.** A device is reached over TCP, so an emulator's ADB listener is expected to work:

- `TcpTransport` connects to a `host:port` endpoint with the platform's sockets, resolves the host with `getaddrinfo`, bounds the connect with `select`, and disables Nagle. Every service works over it unchanged, because `Session` owns the framing.
- Public API: `TcpTransport::open(endpoint)` returning a `Result<TcpTransport>`, mirroring `UsbTransport::open(id)`.

This completes the initial scope.

**Slice 8 — complete.** Two devices of the same model are told apart by their USB serial:

- `DeviceId` carries an optional serial, matched against the device's USB `iSerial` string descriptor, and a selector with no ids matches by serial alone.
- `UsbTransport::list()` enumerates the attached ADB devices with their serials, and `Connection::device_serial()` reports the banner's `serialno` for a device whose descriptor has none.
- Public API: `DeviceId::parse("VID:PID")`, `DeviceId::parse("serial:<serial>")`, and `UsbTransport::list()`.

**Slice 9 — complete.** The 1.0 hardening, then the release:

- `Transport::serial()` reports the device serial that `adb devices` prints, and `run` falls back to the v1 `shell` service without `shell_v2` (blockers 31 and 32).
- The CNXN banner names only the features the library acts on.
- `project(VERSION)` is `1.0.0`, [`CHANGELOG.md`](../CHANGELOG.md) records the release, and `v1.0.0` tags it.

**Slice 10 — complete.** The 2.0 hardening, then the release:

- `Connection::close()` and `Connection::is_open()` close the link and record it, so a later `send`/`receive` reports a `Transport` error instead of touching a closed transport ([issue #11](https://github.com/promethea156/adbcpp/issues/11)).
- `connect_with_retry(open, transport, ...)` owns the open-and-handshake loop with a bounded exponential backoff, which the examples use, and which is also how a dropped link is reconnected ([issue #12](https://github.com/promethea156/adbcpp/issues/12)).
- `Session` is move-only and records whether it is closed, so a moved-from session does not close the transport its successor uses. This is the **BREAKING** change that makes the release `2.0.0`.
- `adbcpp_demo_multi_example` runs the guided tour on every attached device at once, one thread per device.
- `2.0.1` adds [`00-start-here.md`](00-start-here.md) and states the compatibility floor in [`01-objective.md`](01-objective.md).

**Next:** The rest of [Future Improvements](#future-improvements), tracked as [issues](https://github.com/promethea156/adbcpp/issues).

## Proposed Order for What Remains

A proposal, not a commitment. Nothing below blocks the next item, and any of it can move. Each entry is an open issue, ordered by the priority in its title (P2 first).

| Order | Work | Why here |
| --- | --- | --- |
| 1 | Verify the build and tests on Linux and macOS ([#26](https://github.com/promethea156/adbcpp/issues/26), [#25](https://github.com/promethea156/adbcpp/issues/25)) | P2. A real machine confirms what CI only compiles, and [issue #2](https://github.com/promethea156/adbcpp/issues/2)'s acceptance needs it. |
| 2 | State the minimum supported Android version ([#4](https://github.com/promethea156/adbcpp/issues/4)) | P5. The floor is documented but unverified on a device without `shell_v2`. |
| 3 | Use `sendrecv_v2` for `pull` and `push` ([#1](https://github.com/promethea156/adbcpp/issues/1)) | P6. A transfer is never compressed today. |
| 4 | Replace libusb with platform-native USB APIs ([#2](https://github.com/promethea156/adbcpp/issues/2)) | P7. Removes the only third-party runtime dependency and its license obligation. |
| 5 | Decide whether a target needs ADB's TLS handshake ([#5](https://github.com/promethea156/adbcpp/issues/5)) | P8. Only matters for a target that demands the handshake. |

Optional logging ([#3](https://github.com/promethea156/adbcpp/issues/3)) was the P3 item, the non-blocking poll ([#6](https://github.com/promethea156/adbcpp/issues/6)) the P4 item, and both P9 items, the header/payload check ([#23](https://github.com/promethea156/adbcpp/issues/23)) and the separate streams ([#22](https://github.com/promethea156/adbcpp/issues/22)), are now done, so none is listed.

## Slice 0 — Walking Skeleton

Prove the pipeline end-to-end without a real device.

- Directory layout (`include/`, `src/`, `tests/`, `examples/`, `docs/`).
- CMake project targeting C++20 with install/export support, cross-platform (Linux, Windows, macOS).
- Sample project under `examples/`, built against the library: it serves as user-facing examples **and** as a manual integration harness for exercising features against a real device during development.
- Doxygen configuration and documentation build target.
- Unit test framework: [Catch2](https://github.com/catchorg/catch2) (v3), wired into CTest.
- CI building and testing on all three platforms.
- `Transport` interface plus an in-memory **mock transport**.
- A trivial end-to-end protocol path driven by the mock, with unit tests.

**Acceptance:** CI is green on all platforms; a test connects the mock transport through the protocol layer and back.

## Slice 1 — Shell over USB

The first real device capability: connect over USB and run a shell command.

- USB transport via `libusb`, linked dynamically as an optional backend (see [USB Backend](01-objective.md#usb-backend)), including the quirk where the payload is a separate transfer from the header.
- ADB message header: `command`, `arg0`, `arg1`, `data_length`, `data_crc32`, `magic`; little-endian (de)serialization.
- `CNXN` handshake and system-identity string.
- AUTH type 2 (sign the token) and type 3 (send the public key, which triggers the on-device approval prompt).
- Stream lifecycle: `OPEN` / `OKAY` / `WRTE` / `CLSE`.
- Shell service (`shell:<command>`) with output capture.
- Public helper: run a command, return stdout/stderr/exit code.

**Acceptance:** run `echo hello` on a real USB device and capture the output.

## Slice 2 — List Files

Add the `sync` service and directory enumeration.

- `sync:` stream setup.
- `LIST` / `DENT` parsing, including `LIS2` / `DNT2` for large files.
- Public API: `list(path)`.

**Acceptance:** list a directory on a real device and return structured entries.

## Slice 3 — Pull

- Sync `RECV` (chunked `DATA` + `DONE`).
- Public API: `pull(remote, local)`.

**Acceptance:** pull a file from a real device and verify its contents.

## Slice 4 — Push

- Sync `STAT` and `SEND` (chunked `DATA` + `DONE`).
- Public API: `push(local, remote)`.

**Acceptance:** push a file to a real device and verify its contents.

## Slice 5 — Install / Uninstall

- Push APK to `/data/local/tmp`.
- Run `pm install` / `pm uninstall` over the shell service.
- Public API: `install(apk)`, `uninstall(package)`.

**Acceptance:** install and uninstall an APK on a real device. Met with a stock APK
pulled from the device, uninstalled, and installed again from the pulled copy. The
device test runs that round trip when `ADBCPP_TEST_APK` and `ADBCPP_TEST_PACKAGE`
name a disposable APK, and always checks the two failure paths, which touch no
package.

## Slice 6 — App Control

- Launch via `am start`, close via `am force-stop`.
- Running check via `pidof`, which answers both cases directly: exit code 0 with the pid when the process runs, exit code 1 with no output when it does not. It matches a process name rather than a package name, so the answer is an approximation.
- Public API: `launch(package)`, `close(package)`, `is_running(package)`.

**Acceptance:** launch, detect, and close an app on a real device.

## Slice 7 — TCP Transport

The last piece of the [initial scope](01-objective.md#initial-scope), "connection management": reaching a device that is not on the other end of a USB cable.

- A `TcpTransport` over a socket, implementing the same `Transport` interface as `UsbTransport`, so every service works over it unchanged. An emulator's ADB listener (`localhost:5555`) is the common case.
- Public API: `TcpTransport::open(endpoint)`, mirroring `UsbTransport::open(id)`.

**Acceptance:** run `echo hello` and a file round-trip against a device over TCP. Verified against a real `tcpip` device, and covered device-free by a loopback listener in `tcp_test`. An emulator was not itself run; its ADB listener speaks the same plaintext protocol, so it is expected to behave the same.

The transport uses the platform's sockets, so unlike the USB backend it has no third-party dependency and the core does not pull libusb in. The connect is made non-blocking and bounded by `select`, so an unreachable host is noticed, and the socket uses `TCP_NODELAY`, because the header and its payload are separate writes and Nagle would coalesce them; `adb` disables it for the same reason.

Two things this slice deliberately does **not** add. The adb *server* protocol's `host:connect` and `host:disconnect` services live on port 5037 and are excluded by the [key constraint](01-objective.md#key-constraint), so there is no `connect(endpoint)` or `disconnect(endpoint)` to implement. And silent authentication, the `adbkey`/`adbkey.pub` handling, and the fingerprint it needs were already delivered by Slice 1.

This slice completes the initial scope. What remains after it is in [Future Improvements](#future-improvements).

## Slice 8 — Select a Device by Serial

The last gap for the intended use case, [working with several devices](#working-with-several-devices-in-parallel): telling two devices apart when they are the same model.

- `DeviceId` gains an optional `serial`, matched against the device's USB `iSerial` string descriptor, which is the serial `adb` prints in `adb devices`. A selector with no ids matches by serial alone, so the model need not be known.
- `UsbTransport::list()` enumerates the attached ADB devices, each with its serial, so a caller can discover one.
- `Connection::device_serial()` exposes the banner's `serialno` field for a device whose descriptor has none.
- Public API: `DeviceId::parse("VID:PID")` and `DeviceId::parse("serial:<serial>")`, and `UsbTransport::list()`.

**Acceptance:** `examples/multi` lists two attached devices and opens each by its own serial; the same serial also works as a selector on `examples/usb`.

The `serialno` field of the system identity string, `<systemtype>::<serialno>::<banner>`, is empty on the two test devices, whose serial is the descriptor; `parse_serial` reads it for the devices that do fill it in. Reading a descriptor serial needs the device opened, which is why `find_device` only does it when a serial was asked for, and why the WinUSB single-handle rule (blocker 28) makes the two device opens sequential rather than nested.

## Slice 9 — Release 1.0

The initial scope is complete, so this slice is not new capability: it closes the API and compatibility gaps that would be expensive or misleading to change once the version is fixed at 1.0, and then performs the release itself.

- **The `adb devices` serial on the transport.** `Connection::device_serial` returns the banner `serialno`, which is empty on current devices, so a caller reading it gets nothing. Add `Transport::serial()`, returning the transport's identity (the USB `iSerial` descriptor, or the TCP endpoint), and have `device_serial` prefer it and fall back to the banner field. This is the one change here that alters a public type's layout, so it belongs before 1.0.
- **Fall back to `shell:` when `shell_v2` is absent.** `run` opens `shell,v2,raw:` and requires the feature, while `list` and `stat` already fall back to their v1 forms. Open `shell:<command>` when the device did not advertise `shell_v2`, take the combined output as the raw bytes, and report exit code 0, matching adb (the v1 shell has no exit packet; AOSP's own v1 path reports 0). The v1 path can be exercised on a current device by forcing the service string, so it needs no old device to test.
- **Advertise only what is implemented.** The host banner claims `sendrecv_v2` with brotli, lz4, and zstd, and services that are never opened (`abb`, `apex`, `remount_shell`, `track_app`, `devraw`, `server_status`, ...). Either implement `sendrecv_v2` for `pull`/`push` or trim the list to the features the library acts on, so a peer cannot rely on a claim that is not honored.
- **Release chores.** Bump `project(VERSION)` to `1.0.0` (`SOVERSION` follows to 1), add a `CHANGELOG.md` generated from the Conventional Commits history, and tag `v1.0.0`. ([issue #7](https://github.com/promethea156/adbcpp/issues/7))

**Acceptance:** `device_serial()` returns the same string `adb devices` prints for a USB device; `run` works against a device that does not advertise `shell_v2` (forced with `shell:`) as well as one that does; the banner claims nothing unimplemented; and a `v1.0.0` tag builds and passes the suite on all three CI platforms.

## Cross-Cutting Concerns

Obligations that run through every slice, with the current state of each.

- **Error handling**: every operation that can fail returns a `Result<T>`; nothing in the library throws, and third-party exceptions are caught at the boundary. The rule, the types, and the shape of each command's answer are in [`07-error-model.md`](07-error-model.md).
- **Testing**: unit tests per module, driven by the mock transport, plus one integration test against a real device. Device-dependent tests live in `adbcpp_device_tests`; when no matching USB device is present they exit with code 77 so CTest reports them as skipped rather than failed, and the USB example prints a warning and exits successfully in the same case.
- **Logging**: implemented as `adbcpp/log.hpp`. It is opt-in and off until `set_logger` installs a sink, configurable per level, and never logs key material or a payload: a frame is logged with its command, arguments, and length only, and the service a stream is opened for is the only payload-derived detail, at `Trace`. `tests/log_test.cpp` covers the CNXN/AUTH flow and pins that none of the key material reaches the sink. ([issue #3](https://github.com/promethea156/adbcpp/issues/3))
- **Thread safety**: `Transport`, `Connection`, `Stream`, and `Key` each state that they are not thread-safe, and that a caller must serialize concurrent use. Independent objects share no state, so the model for several devices is one thread per device. One thread can also drive several devices, by waiting on their transports with `adbcpp::wait_readable` (see [Working with Several Devices in Parallel](#working-with-several-devices-in-parallel)).
- **Documentation**: Doxygen comments on every public declaration, and the reasoning behind each protocol decision written down in [`04-blockers.md`](04-blockers.md).
- **Compatibility**: the minimum supported Android is **7.0 (API 24)** and the minimum ADB protocol version is `0x01000001`. The v2 `shell`, `LIST`, and `STAT` forms are used when the device advertises them, with their v1 forms as the fallback. ([issue #4](https://github.com/promethea156/adbcpp/issues/4))

## Open Questions

- **Does a target need ADB's TLS handshake?** Modern ADB encrypts the host-to-server link on port 5037; over USB the direct protocol has none, and an emulator's ADB listener and a `tcpip` device both accept plaintext. So this only matters for a target that demands the handshake, which is a [future improvement](#other-improvements). mbedTLS can provide it if so. ([issue #5](https://github.com/promethea156/adbcpp/issues/5))

## Future Improvements

Each item below is tracked as an issue in the [issue tracker](https://github.com/promethea156/adbcpp/issues).

### Working with Several Devices in Parallel

The intended use case is driving several devices at once, and the default model is
**one thread per device**. It needs no change to `Transport`: every `UsbTransport`
has its own libusb context, and `Connection`, `Stream`, and `Key` are independent
and documented as one-object-per-thread, so several connections on several threads
share no state. `Transport::read` therefore stays blocking, and `examples/multi`
drives two devices on two threads.

**Done.** One thread can also drive several devices. `Transport::wait_readable` waits
for readability with a timeout, and the `adbcpp::wait_readable` helper waits on
several transports at once and returns the readable one, which the caller then reads.
`TcpTransport` waits with `select`, `UsbTransport` with one bulk transfer bounded by
the timeout, and the mock reports whether bytes are queued. `examples/poll` drives two
loopback devices from one thread and runs in CI.
([issue #6](https://github.com/promethea156/adbcpp/issues/6))

Each device also needs its own connection: the WinUSB driver on the test device admits
a single handle, so the same device cannot be opened twice, not even from two
processes (blocker 28). Two devices are therefore needed, and `examples/multi` opens
two and reports clearly when only one is present.

A USB 3 device also resets its link around the open and stalls the first write
(blocker 29), so `connect_with_retry` retries the whole open and handshake; `adb` does the
same at a lower level by sending the CNXN twice.

Selection by serial was the last gap, and [Slice 8](#slice-8--select-a-device-by-serial)
closes it: `DeviceId` matches the device's USB `iSerial` string descriptor, and
`UsbTransport::list` enumerates the serials, so two identical devices no longer
both resolve to the first match.

`examples/multi` is the acceptance: it lists the attached devices, opens two by their
own serial, each on its own thread, and both run `echo hello` and a file round trip.

### Other Improvements

- **Use `sendrecv_v2` for `pull` and `push`.** The v1 `RECV`/`SEND` forms are sent today, so a transfer is never compressed. [Slice 9](#slice-9--release-10) removed the false `sendrecv_v2` claim from the banner; implementing the v2 forms would let a transfer use brotli, lz4, or zstd. ([issue #1](https://github.com/promethea156/adbcpp/issues/1))
- **Give `Connection` an explicit `disconnect`.** **Done.** Closing a link is `transport->close()` on the borrowed transport, and a `Connection` records no dead state, so a closed connection still looks live. `Connection::close()` now closes the transport and marks the connection dead, so a later `send`/`receive` reports a `Transport` error, and `Connection::is_open()` reports the state. ([issue #11](https://github.com/promethea156/adbcpp/issues/11))
- **Reconnect a dropped or reset link.** **Done.** There is no `reconnect()` and `connect` does not retry, so a dropped TCP link or a USB 3 link reset (blocker 29) needs the same manual close, reopen, and re-handshake that the examples hand-roll. `adbcpp::connect_with_retry(open, transport, ...)` now owns that loop with a bounded exponential backoff, and the examples call it instead. ([issue #12](https://github.com/promethea156/adbcpp/issues/12))
- **Separate stdout and stderr in `CommandResult`.** **Done.** `run` merged the two shell_v2 streams, so a caller could not tell which one a line came from. `CommandResult` now carries `standard_output` and `error_output` as well as the merged `output`, and the v1 fallback leaves the two separate fields empty, since it does not separate them. ([issue #22](https://github.com/promethea156/adbcpp/issues/22))
- **Check a message header against the payload it writes.** **Done.** `Message` and `Session::send` take the header and payload separately, so a header could advertise a `data_length` that did not match the bytes written, which is how blocker 13 stalled the `OPEN`. `Session::send` now checks that `header.data_length` equals `payload.size()` and reports a mismatch as an `InvalidArgument` before it writes anything. ([issue #23](https://github.com/promethea156/adbcpp/issues/23))
- Replace the dynamically-linked libusb backend with platform-native USB APIs (WinUSB, IOKit, `usbfs`) to remove the third-party dependency and its license obligations. See [USB Backend](01-objective.md#usb-backend). ([issue #2](https://github.com/promethea156/adbcpp/issues/2))
