# Roadmap

This document outlines the planned implementation of `adbcpp` as a series of **vertical slices**. Each slice delivers a thin, end-to-end capability that spans every layer it needs (protocol, transport, session, service, public API), and ships with its own tests, documentation, and example. Layers are fleshed out incrementally, only as far as each slice requires.

This is deliberate: rather than completing an entire layer before anything is runnable, every slice ends with something demonstrable against a real device (except Slice 0).

## Current Status

**Slice 0 — complete.** CMake project, Catch2 tests, sample, Doxygen, CI.

**Slice 1 — complete.** `echo hello` runs over USB against a real device and its output is captured:

- USB transport: ADB interface + bulk endpoints, `clear_halt` on open, header/payload as separate transfers.
- Handshake: `CNXN` (advertising `host::features=<list>` and a 1 MiB maximum payload) → `AUTH` token → signed reply → device `CNXN` (protocol `0x01000001`).
- Keys are persisted at `~/.android/adbkey` (PKCS#8 PEM) and `~/.android/adbkey.pub` (ADB format).
- The token is signed with the private key (`AUTH` type 2, PKCS#1 v1.5/SHA-1). adbd verifies with `RSA_verify(NID_sha1, token, ...)`, so the token is signed directly as the SHA-1 digest and is not re-hashed. If the device rejects the signature, the public key is offered (`AUTH` type 3) so the user can authorize it, exactly like adb.
- Stream lifecycle: `OPEN` / `OKAY` / `WRTE` / `CLSE`, with `shell_v2` stdout/stderr/exit parsing.

Two bugs found while getting `OPEN` working:

| Field | adb | ours (before) |
| --- | --- | --- |
| `arg0` (local id) | `2` | `1` |
| `arg1` (send buffer) | `0` (no `delayed_ack` advertised) | `0x40000` (we advertised `delayed_ack`) |
| payload | `shell,v2,raw:echo hello\0` | same |

Matching adb's values (local id `2`, `delayed_ack` removed from the feature list, `arg1` `0`) did **not** fix it. Instrumenting the session showed the real cause: the `OPEN` header advertised `data_length=24`, but `Stream`'s constructor passed the payload only to `make_message` and not to `Connection::send`, so the 24 payload bytes were never written. The payload is now passed to `send` as well.

A third bug blocked silent authentication: `Key::sign` hashed the token with SHA-1 and then signed that hash as the digest, double-hashing it, so adbd rejected every signature and showed the authorization prompt on every run. `Key::sign` now signs the token directly as the digest, matching adb's `RSA_sign(NID_sha1, token, ...)`. This was confirmed with a USBPcap capture of one adb session and one `adbcpp` session; see blockers 10 and 16 in `04-blockers.md`.

A fourth bug was found later, while testing Slice 2: `run` read the shell_v2 exit code from the exit packet's length instead of its data, so every command reported exit code `1`. The length is always `1` and the status is the single data byte, matching adbd's `data()[0] = exit_code; Write(kIdExit, 1)`. `run` now reads the status from the data; see blocker 19 in `04-blockers.md`.

**Slice 2 — complete.** A directory is listed over the `sync` service and its entries are returned as structured data:

- `sync:` stream setup, which puts the stream in a binary mode that differs from the ADB protocol.
- `LIST` request and `DENT` response parsing, using the v2 `LIST`/`DNT2` form when the device advertises `ls_v2` (the device does, so the full POSIX metadata and per-entry errors are available).
- Public API: `list(connection, path)` returning `DirEntry` values with `name`, `mode`, `size`, and `mtime`.
- `Stream::read` reads an exact byte count across `WRTE` messages, which the length-prefixed sync responses need.

Two more protocol details were found while getting `list` working:

- The device acknowledges the `LIST` `WRTE` with an `OKAY`, so `Stream` must skip the `OKAY` frames that carry no data.
- The device may send a second `CLOSE` for a previous stream while a new one opens, so `Stream` must ignore frames whose `arg1` is not its own local id.

The wire format is documented in [`06-sync-protocol.md`](06-sync-protocol.md), and `examples/sync/main.cpp` runs the whole exchange against the mock transport, so `list` is exercised with no device attached.

**Slice 3 — complete.** A file is copied from the device with the `sync` `RECV` request:

- `RECV` request and the `DATA` chunks that follow, ending with `DONE`.
- Each chunk is written to the local file as it arrives, so a file of any size costs no more memory than a small one.
- Public API: `pull(connection, remote, local)`.

A transfer's `DONE` is a `sync_data` record, not the DENT struct that ends a listing, so only its size is read; see [`06-sync-protocol.md`](06-sync-protocol.md).

**Slice 4 — complete.** A file is copied to the device with the `sync` `SEND` request:

- `STAT` request and its v1/v2 responses, used to tell whether the destination is a directory.
- `SEND` request, whose path is a `<path>,<mode>` spec, followed by the same `DATA` chunks as `RECV` and a `DONE` whose size is the file's modification time.
- The device answers the final `DONE` with `OKAY`, unlike a listing or a transfer.
- Public API: `stat(connection, path)` and `push(connection, local, remote)`.

`stat` follows symbolic links, so a symlink to a directory is correctly reported as a directory. It returns a missing path as an empty result rather than an error, because the device reports it inside the response.

**Slice 5 — complete.** An APK is installed and a package is uninstalled:

- `install` pushes the APK into the device's `/data/local/tmp` with `push`, runs `pm install` over the shell service, and removes the pushed APK whether the install worked or not.
- `uninstall` runs `pm uninstall`; `keep_data` adds `-k`, which keeps the package's data.
- Public API: `install(connection, apk, options)` and `uninstall(connection, package, keep_data)`, both returning a `PackageResult`.
- `options` are passed to `pm install` verbatim, so `-r` reinstalls a package in place and keeps its data.

Nothing was needed at the protocol layer: the slice is composition, as the roadmap predicted. `push` is the `sync` `SEND` request and `pm install` is a shell command, so both already existed, and `pm` reports its outcome through the exit code that blocker 19 fixed.

Two details were found while getting it working. The APK path and the package name are single-quoted, because the device runs the command through `sh -c`, exactly as adb's `escape_arg` does. And a package manager that rejects a request is a normal answer rather than an error, so it is returned as `success == false` with the device's output; the two commands report a rejection differently (blocker 25).

**Next:** Slice 6 — app control.

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
- ADB message header: `command`, `arg1`, `arg2`, `data_length`, `data_crc32`, `magic`; little-endian (de)serialization; CRC32 and magic validation.
- `CNXN` handshake and system-identity string.
- AUTH type 3 (send public key) to trigger the on-device approval prompt.
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
- Running check via `pidof` / `dumpsys` / `ps`.
- Public API: `launch(package)`, `close(package)`, `is_running(package)`.

**Acceptance:** launch, detect, and close an app on a real device.

## Slice 7 — Connect / Disconnect & Silent Auth

- TCP transport.
- AUTH type 2 (sign the token) using an RSA private key for silent reconnection.
- ADB key handling (`adbkey` / `adbkey.pub`) and fingerprint format.
- Public API: `connect(endpoint)`, `disconnect(endpoint)`.

**Acceptance:** reconnect to a previously authorized device without an on-device prompt.

## Cross-Cutting Concerns

Apply throughout every slice.

- **Error handling**: consistent error type/result model; no exceptions leaking across the API boundary unless documented.
- **Testing**: unit tests per module plus integration tests against a real device/emulator where possible. Device-dependent tests live in `adbcpp_device_tests`; when no matching USB device is present they exit with code 77 so CTest reports them as skipped rather than failed. The USB example prints a warning and exits successfully in the same case.
- **Logging**: optional, configurable, never leaks sensitive data (keys, payloads).
- **Thread safety**: document which objects are safe to share.
- **Documentation**: Doxygen comments kept current as the API grows.

## Open Questions

- Scope of TLS/encrypted ADB transport support (required by newer Android versions).
- Minimum supported Android version.

## Future Improvements

- Replace the dynamically-linked libusb backend with platform-native USB APIs (WinUSB, IOKit, `usbfs`) to remove the third-party dependency and its license obligations. See [USB Backend](01-objective.md#usb-backend).
