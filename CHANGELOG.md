# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed

- `adbcpp_demo_example` and `adbcpp_demo_multi_example` wake the device with
  `input keyevent 82` after `224`, so the tour dismisses the keyguard the wake
  leaves.

### Fixed

- `launch` runs `monkey -p <package> -c android.intent.category.LAUNCHER 1`
  instead of `am start -W <package>`, so an app whose launcher a bare
  `am start <package>` cannot resolve is started (blocker 33).

### Added

- `CommandResult::standard_output` and `CommandResult::error_output`, which report
  the `shell_v2` service's standard output and standard error separately, while
  `CommandResult::output` still merges them in the order the device produced them.
  The v1 `shell` service leaves the two separate fields empty.
- `Session::send` checks that `header.data_length` equals `payload.size()` and
  reports a mismatch as an `InvalidArgument` before writing anything, so a header
  cannot leave the device waiting for bytes that never arrive (blocker 13).
- `adbcpp::wait_readable(transports, timeout)`, which waits on several
  transports at once and returns the readable one, and
  `Transport::wait_readable(timeout)`, which each transport implements:
  `TcpTransport` with `select`, `UsbTransport` with one bounded bulk transfer, and
  the mock with its queued bytes. `examples/poll` drives two loopback devices from
  one thread.
- `pull` and `push` use the v2 `RECV`/`SEND` forms with a codec when the build
  has one and the device advertised it, so a transfer can compress.
  `SyncCompression` selects the codec per call (`Auto` prefers zstd, then lz4, then
  brotli), and `Auto` falls back to the v1 forms, which still work for a device
  without the feature. zstd (`ADBCPP_BUILD_COMPRESSION`), lz4 (`ADBCPP_BUILD_LZ4`),
  and brotli (`ADBCPP_BUILD_BROTLI`) are each fetched with `FetchContent` and linked
  privately, and the banner names only the codecs that are built in.
  `docs/09-sendrecv-v2.md` is the plan.
- `adbcpp_usb_example` takes `--compression <auto|none|zstd|lz4|brotli>` for
  `--pull` and `--push`, so the v1 form and each codec can be selected from the
  command line, and `tools/bench-compression.ps1` times each mode against `none` and
  prints the speedup. `docs/06-sync-protocol.md` records the measured result, and
  `docs/00-start-here.md` explains the three codecs and why `Auto` prefers zstd.

## [2.1.0] - 2026-09-21

### Added

- Optional, level-configurable logging in [`include/adbcpp/log.hpp`](include/adbcpp/log.hpp)
  (`set_logger`, `clear_logger`, `is_logging`, `log`), wired into `Session`,
  `Connection`, `Stream`, and the USB and TCP transports. It reports frames, retries,
  and state changes, and never logs key material or a payload.

### Changed

- `docs/03-roadmap.md` records the 2.0 release, replaces the completed "Proposed
  Order for What Remains" table with the current open backlog, and marks logging as
  implemented.
- `docs/05-usage.md` documents logging, and `docs/00-start-here.md` states the
  current version.

### Fixed

- The CI release job publishes the tag's own changelog section instead of the empty
  `[Unreleased]` section, which is what v2.0.0 and v2.0.1 were published with.

## [2.0.1] - 2026-09-21

### Added

- [`docs/00-start-here.md`](docs/00-start-here.md), a plain-language tour of the
  project for a first-time reader, linked from the README.

### Changed

- `docs/01-objective.md` states the minimum supported Android version (7.0, API 24)
  and ADB protocol version (`0x01000001`).
- `docs/03-roadmap.md` lists issues #22 and #23.

## [2.0.0] - 2026-09-21

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

### Changed

- **BREAKING**: `Session` is now move-only and records whether it is closed, so a
  moved-from session does not close the transport its successor uses.
- The examples use `connect_with_retry` and `connection->close()`.
- The README states the minimum supported Android (7.0, API 24) and ADB protocol
  version (`0x01000001`), and the platform support section distinguishes CI
  coverage from hand verification.

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

[Unreleased]: https://github.com/promethea156/adbcpp/compare/v2.1.0...HEAD
[2.1.0]: https://github.com/promethea156/adbcpp/releases/tag/v2.1.0
[2.0.1]: https://github.com/promethea156/adbcpp/releases/tag/v2.0.1
[2.0.0]: https://github.com/promethea156/adbcpp/releases/tag/v2.0.0
[1.0.0]: https://github.com/promethea156/adbcpp/releases/tag/v1.0.0
