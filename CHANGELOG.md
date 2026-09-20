# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- `Connection::close()` and `Connection::is_open()`, so a caller can close a link
  and know it; a later `send`/`receive` reports a `Transport` error instead of
  touching the closed transport.
- `adbcpp_demo_multi_example`, which runs the guided tour on every attached
  device at once, one thread per device, so the install, launch, and uninstall steps
  overlap. It uninstalls the package on each device first and again at the end.
- `connect_with_retry(open, transport, ...)`, which opens a transport and performs
  the handshake with a bounded exponential backoff, and is also how a dropped link
  is reconnected. The examples use it instead of their own retry loop.

## [1.0.0] - 2026-09-18

The initial release: the whole [initial scope](docs/01-objective.md#initial-scope)
over USB and TCP, with no dependency on the ADB server or the `adb` binary.

### Added

- A `Transport` over ADB's USB interface (`UsbTransport`), backed by libusb and
  linked dynamically as an optional backend.
- The CNXN/AUTH handshake, the `adbkey`/`adbkey.pub` key pair, and the
  `OPEN`/`OKAY`/`WRTE`/`CLSE` stream lifecycle.
- `run(connection, command)`, which captures a shell command's combined output,
  its exit code, and whether it worked.
- `list(connection, path)` over the `sync` `LIST` request, returning directory
  entries, and the `sync` wire format in [`06-sync-protocol.md`](docs/06-sync-protocol.md).
- `pull(connection, remote, local)` over the `sync` `RECV` request, and
  `push(connection, local, remote)` over `STAT` and `SEND`.
- `install(connection, apk, options)` and `uninstall(connection, package)`, built
  on `push` and `run`.
- `launch`, `close`, and `is_running` for application control and status.
- A `Result<T>` error model, so nothing in the library throws.
- `TcpTransport`, a socket transport for an emulator or a `tcpip` device, with no
  third-party dependency.
- `DeviceId::serial` and `UsbTransport::list()`, so two devices of the same model
  are told apart by their USB serial.
- `examples/`, `LEARNING.md`, and the design documents under `docs/`.

### Changed

- `Connection`, `Stream`, and `Key` state that they are not thread-safe, and the
  model for several devices is one thread per device.

### Fixed

- The payload checksum is adb's byte sum, not a CRC-32.
- The AUTH token is signed as the SHA-1 digest and not re-hashed.
- The CNXN banner advertises the host features, without which the device treats the
  host as supporting nothing.
- The OPEN payload is sent over the stream and is null-terminated.
- A received header is validated before its payload is read.
- `run` falls back to the v1 `shell` service when the device has no `shell_v2`,
  and `pull`/`push` fall back to the v1 `sync` forms.

### Performance

- A `push` chunk is written as one message.

[1.0.0]: https://github.com/promethea156/adbcpp/releases/tag/v1.0.0
