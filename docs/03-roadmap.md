# Roadmap

This document outlines the planned implementation of `adbcpp` as a series of **vertical slices**. Each slice delivers a thin, end-to-end capability that spans every layer it needs (protocol, transport, session, service, public API), and ships with its own tests, documentation, and example. Layers are fleshed out incrementally, only as far as each slice requires.

This is deliberate: rather than completing an entire layer before anything is runnable, every slice ends with something demonstrable against a real device (except Slice 0).

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

**Acceptance:** install and uninstall an APK on a real device.

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
- **Testing**: unit tests per module plus integration tests against a real device/emulator where possible.
- **Logging**: optional, configurable, never leaks sensitive data (keys, payloads).
- **Thread safety**: document which objects are safe to share.
- **Documentation**: Doxygen comments kept current as the API grows.

## Open Questions

- Crypto dependency for RSA signing (OpenSSL vs. a lighter alternative).
- Scope of TLS/encrypted ADB transport support (required by newer Android versions).
- Minimum supported Android version.

## Future Improvements

- Replace the dynamically-linked libusb backend with platform-native USB APIs (WinUSB, IOKit, `usbfs`) to remove the third-party dependency and its license obligations. See [USB Backend](01-objective.md#usb-backend).
