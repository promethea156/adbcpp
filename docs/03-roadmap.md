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

Nothing was needed at the protocol layer: the slice is composition again, as the roadmap predicted. `am start`, `am force-stop`, and `pidof` are shell commands, so both already existed.

**Next:** settle the threading model for several devices, then Slice 7 — TCP transport.

## Proposed Order for What Remains

A proposal, not a commitment. Nothing below blocks the next slice, and any of it can move.

| Order | Work | Why here |
| --- | --- | --- |
| 1 | Apply the [error model](07-error-model.md) | **Done.** The whole library returns `Result<T>`, so Slice 6 adds its three functions in the new shape rather than converting them later. |
| 2 | [Slice 6 — app control](#slice-6--app-control) | **Done.** Composition again: `am start`, `am force-stop`, and `pidof` are shell commands. |
| 3 | Validate the received header | **Done.** `Session::receive` checks `magic`, bounds `data_length`, and verifies a non-zero CRC, closing the transport on a framing error. |
| 4 | State thread safety for `Connection`, `Stream`, and `Key` | **Done.** Each states that it is not thread-safe and must be serialized by the caller, matching `Transport`. |
| 5 | Settle the [threading model for several devices](#working-with-several-devices-in-parallel) | One thread per device works today; a non-blocking read, if needed, must land before Slice 7 so both transports implement it once. |
| 6 | [Slice 7 — TCP transport](#slice-7--tcp-transport) | Completes the initial scope. |
| 7 | [Select devices by serial](#working-with-several-devices-in-parallel) | The enabler for the intended use case, once the threading model is fixed and TCP is in. |
| 8 | The rest of [Future Improvements](#other-improvements) | Logging, the `shell_v2` fallback, and `sendrecv_v2` each matter only once a caller needs them. |

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

**Acceptance:** run `echo hello` and a file round-trip against an emulator over TCP.

Two things this slice deliberately does **not** add. The adb *server* protocol's `host:connect` and `host:disconnect` services live on port 5037 and are excluded by the [key constraint](01-objective.md#key-constraint), so there is no `connect(endpoint)` or `disconnect(endpoint)` to implement. And silent authentication, the `adbkey`/`adbkey.pub` handling, and the fingerprint it needs were already delivered by Slice 1.

This slice completes the initial scope. What remains after it is in [Future Improvements](#future-improvements).

## Cross-Cutting Concerns

Obligations that run through every slice, with the current state of each.

- **Error handling**: every operation that can fail returns a `Result<T>`; nothing in the library throws, and third-party exceptions are caught at the boundary. The rule, the types, and the shape of each command's answer are in [`07-error-model.md`](07-error-model.md).
- **Testing**: unit tests per module, driven by the mock transport, plus one integration test against a real device. Device-dependent tests live in `adbcpp_device_tests`; when no matching USB device is present they exit with code 77 so CTest reports them as skipped rather than failed, and the USB example prints a warning and exits successfully in the same case.
- **Logging**: not implemented. When it is, it must be optional, configurable, and must never log keys or payloads.
- **Thread safety**: `Transport`, `Connection`, `Stream`, and `Key` each state that they are not thread-safe, and that a caller must serialize concurrent use.
- **Documentation**: Doxygen comments on every public declaration, and the reasoning behind each protocol decision written down in [`04-blockers.md`](04-blockers.md).

## Open Questions

- **Does a target need ADB's TLS handshake?** Modern ADB encrypts the host-to-server link on port 5037; over USB the direct protocol has none, and an emulator's ADB listener accepts plaintext. So this only matters for a target that demands the handshake, which belongs with [Slice 7](#slice-7--tcp-transport). mbedTLS can provide it if so.
- **What is the minimum supported Android version?** Today it is set by `shell_v2`, which `run` requires and has no fallback for, and by the v1 `LIST`/`STAT` forms, which are used when the device does not advertise `ls_v2`/`stat_v2`. The floor should be stated once a device without `shell_v2` has been tried.

## Future Improvements

### Working with Several Devices in Parallel

The intended use case is driving several devices at once. Most of the design already
supports it: every `UsbTransport` has its own libusb context, and `Connection`,
`Stream`, and `Key` are independent and already documented as one-object-per-thread,
so one device per thread works today. Two gaps remain, and one decision.

- **Device identity.** `UsbTransport::open(DeviceId)` matches on vendor and product id
  alone, and `find_device` returns the first match, so two identical devices cannot be
  told apart. Add selection by serial: enumerate, read each device's USB `iSerial`
  string, and let the caller open by serial, as adb does. The device banner's
  `serialno` is the fallback where the descriptor has none.
- **Threading model.** The blocking `Transport::read` leaves one thread per device as the
  only option. If that is acceptable, state it and add a two-thread example. If one
  thread must drive several devices, add a non-blocking read or a poll to `Transport`
  and build the wait on it. This belongs **before** Slice 7's TCP transport, so both
  backends implement the same interface once instead of one being retrofitted.
- **Acceptance.** Two devices, or two emulators over TCP, are opened by serial and
  driven concurrently, and both run `echo hello` and a file round trip.

### Other Improvements

- **Fall back to `shell:` when `shell_v2` is absent.** `run` requires `shell_v2`, while `list` and `stat` already fall back to their v1 forms.
- **Use `sendrecv_v2`, or stop advertising it.** The CNXN banner claims `sendrecv_v2` with brotli, lz4, and zstd, but `pull` and `push` always send the v1 forms, so a transfer is never compressed. The rest of the banner is copied from adb byte-for-byte and therefore also claims services that are never opened (`abb`, `apex`, `remount_shell`, `track_app`, `devraw`, `server_status`, ...); it should be trimmed to what the library implements.
- Replace the dynamically-linked libusb backend with platform-native USB APIs (WinUSB, IOKit, `usbfs`) to remove the third-party dependency and its license obligations. See [USB Backend](01-objective.md#usb-backend).
