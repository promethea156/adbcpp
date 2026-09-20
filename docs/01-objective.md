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

## Compatibility

The minimum supported Android version is **7.0 (API 24)**, and the minimum ADB protocol version is `0x01000001`. The v2 `shell`, `LIST`, and `STAT` forms are used when the device advertises them, with their v1 forms as the fallback. The floor is stated but not yet verified on a device without `shell_v2`; see [issue #4](https://github.com/promethea156/adbcpp/issues/4).

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

## Branching

The repository keeps four long-lived branches, so a change is always made somewhere that says
what it is for:

- `main` is stable. Every commit on it is a release or a change about to be tagged, and a tag
  is only made here. Nothing lands on `main` without passing the whole CI matrix.
- `development` is the integration branch. Feature work merges here first, and it is where the
  slices in [`03-roadmap.md`](03-roadmap.md) are built.
- `release_candidate` is the stabilization branch. It is cut from `development` when a release
  is being prepared, and only fixes for that release land on it until it is merged to `main`.
- `hotfix` is for an urgent fix to a tagged release. It is cut from `main`, fixed, and merged
  back to both `main` and `development`, so the fix is not lost by the next release.

A short-lived branch is named `<type>/<slug>`, using the same types as the commit convention
below, for example `feat/push` or `fix/usb-short-transfer`. It is deleted once it merges.

### Merging

How a branch is merged depends on whether it is long-lived, because a squash gives the target a
**new commit** rather than the source's history:

- A short-lived branch **SHOULD** be squash-merged into its target. The branch is deleted, so the
  rewritten history costs nothing and the target keeps one clean commit per change.
- A merge between two long-lived branches (`development` → `release_candidate` → `main`, and `hotfix`
  back to both) **MUST** be a **merge commit**, not a squash. A squash leaves the source branch
  holding commits that the target does not have, so the two diverge, the next pull request shows
  commits that are already released, and a conflict is guaranteed on the next touch of a shared file.
- If a long-lived branch is nonetheless squash-merged, the target **MUST** be merged back into the
  source immediately afterwards, so the source regains the target's history and the divergence is closed.

The rule in one line: never squash a long-lived branch into another long-lived branch without a
back-merge.

## Releasing

A release is a tag, and the tag is the release: nothing is published that CI has not
built and tested on every platform first. The release is prepared on `release_candidate`,
merged to `main`, and tagged on `main`.

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
