# Objective

## Purpose

This repository aims to provide a **simplified, customized reimplementation of ADB (Android Debug Bridge) as a C++ library**. The library is designed to be embedded directly into C++ applications, giving developers programmatic control over Android devices.

## Key Constraint

The library must operate **without depending on Google's ADB server or the `adb` command-line tool**. There is no `adb.exe` process to spawn, no local server on port 5037, and no external binary required. All device communication is handled internally by the library itself.

## Initial Scope

The first iteration targets a minimal but practical feature set:

- **File transfer**: pull and push files to and from a device.
- **Package management**: install and uninstall applications.
- **File listing**: enumerate files and directories on a device.
- **Connection management**: connect to and disconnect from devices.
- **App control**: launch and close applications.
- **App status**: check whether an application is running.

## Non-Goals (for now)

- Full parity with every `adb` subcommand.
- Support for every Android version or device vendor.
- Serving as a drop-in replacement for the official ADB tooling.

## Technical Requirements

- **Cross-platform**: Linux, Windows, and macOS.
- **Language standard**: C++20.
- **Build system**: CMake.
- **Documentation**: Doxygen.

## USB Backend

Device communication goes through a `Transport` abstraction, which keeps the core library free of any USB dependency.

**Decision (during development):** use **libusb** for the USB transport, linked **dynamically** as an optional backend target. libusb is licensed under LGPL-2.1-or-later, so it must **not** be statically linked into `adbcpp`. The crypto and USB backends are the third-party dependencies that must **not** be statically linked into the core; the core library itself stays BSL-1.0 and adds only `tl::expected`, a header-only library under CC0-1.0. Consumers that need USB opt into the dynamically-linked backend, while those that only need TCP or an emulator do not pull libusb in. libusb is acquired with CMake **FetchContent** via the community [`libusb/libusb-cmake`](https://github.com/libusb/libusb-cmake) build and built as a **shared** library (`LIBUSB_BUILD_SHARED_LIBS=ON`).

**Future improvement:** once most of the implementation is complete, replace libusb with platform-native USB APIs (WinUSB on Windows, IOKit on macOS, `usbfs` on Linux) to remove the third-party dependency and its license obligations entirely.

## Crypto

RSA key generation (the ADB public/private key pair) and token signing use **mbedTLS** (Apache-2.0), acquired with CMake **FetchContent**. This is only required for device authentication. mbedTLS can also provide TLS should the encrypted ADB transport turn out to be required.

## Guiding Principles

- **Self-contained**: no reliance on external ADB components.
- **Embeddable**: usable as a library from other C++ projects.
- **Focused**: implement only what is needed, cleanly.
- **Portable**: consistent behavior and API across all supported platforms.

## Versioning

The project follows [Semantic Versioning 2.0.0](https://semver.org/) (SemVer).

Versions take the form `MAJOR.MINOR.PATCH`:

- **MAJOR** is incremented for incompatible API changes.
- **MINOR** is incremented when functionality is added in a backwards-compatible manner.
- **PATCH** is incremented for backwards-compatible bug fixes.

Pre-release and build metadata **MAY** be appended as `-<pre-release>` and `+<build>` respectively (e.g. `1.0.0-alpha.1`, `1.0.0+build.5`).

Version bumps are derived from commit types, tying SemVer to the [commit message convention](#commit-messages) below:

- `fix` maps to a **PATCH** release.
- `feat` maps to a **MINOR** release.
- A `BREAKING CHANGE` (or `!`), regardless of type, maps to a **MAJOR** release.

## Releasing

A release is a tag, and the tag is the release: nothing is published that CI has not
built and tested on every platform first.

1. Bump `project(VERSION)` in `CMakeLists.txt`.
2. Add the release's section to [`CHANGELOG.md`](../CHANGELOG.md), newest first.
3. Commit with `chore(release): <version>`.
4. Tag it `v<version>` and push the tag. The tag runs the whole CI matrix
   (`.github/workflows/ci.yml`), and the `release` job then publishes the tag's
   changelog section as the GitHub release.

A release is only tagged once the tests pass on all three platforms, and a breaking
change is only released on a **MAJOR** bump, so a tag's version and the changelog
section it publishes never disagree.

## Commit Messages

Commit messages follow the [Conventional Commits 1.0.0](https://www.conventionalcommits.org/en/v1.0.0/#specification) specification.

### Format

```
<type>[optional scope]: <description>

[optional body]

[optional footer(s)]
```

### Types

- `feat`: a new feature.
- `fix`: a bug fix.
- `docs`: documentation only changes.
- `refactor`: a code change that neither fixes a bug nor adds a feature.
- `perf`: a performance improvement.
- `test`: adding or correcting tests.
- `build`: changes to the build system or dependencies (e.g. CMake).
- `ci`: changes to CI configuration.
- `chore`: other changes that don't modify source or test files.
- `style`: formatting changes that don't affect meaning.

### Rules

- A scope **MAY** be provided in parentheses, e.g. `feat(transport): ...`.
- The description follows the colon and a space and is a short summary.
- A body **MAY** follow after one blank line for additional context.
- Breaking changes **MUST** be indicated with a `!` after the type/scope, and/or a `BREAKING CHANGE:` footer.
- Types are case-insensitive in practice; `BREAKING CHANGE` **MUST** be uppercase.

### Examples

```
docs: add commit message standard
feat(sync): implement push command
fix(usb): handle short USB transfers
feat(api)!: rename connect to open

BREAKING CHANGE: `connect` is now named `open`.
```
