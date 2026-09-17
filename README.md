# adbcpp

A simplified, customized reimplementation of **ADB (Android Debug Bridge) as a C++ library**.

`adbcpp` is designed to be embedded directly into C++ applications, giving developers programmatic control over Android devices. It operates **without depending on Google's ADB server or the `adb` command-line tool** — no `adb.exe` process, no local server on port 5037, and no external binary.

> **Status:** early development. Slice 0 (the walking skeleton), Slice 1 (shell over USB), and Slice 2 (file listing over `sync`) are complete. See [`docs/03-roadmap.md`](docs/03-roadmap.md) for the plan.

> **Learning:** want to understand the protocol rather than just use it?
> [`LEARNING.md`](LEARNING.md) is a guided curriculum that uses this
> repository as a worked example, module by module.

## Goals

- **Self-contained**: no reliance on external ADB components.
- **Embeddable**: usable as a library from other C++ projects.
- **Cross-platform**: Linux, Windows, and macOS.
- **Focused**: implement only what is needed, cleanly.

## Initial Scope

The first iteration targets a minimal but practical feature set:

- **File transfer**: pull and push files to and from a device.
- **Package management**: install and uninstall applications.
- **File listing**: enumerate files and directories on a device.
- **Connection management**: connect to and disconnect from devices.
- **App control**: launch and close applications.
- **App status**: check whether an application is running.

## Requirements

- **C++20** compiler
- **CMake** 3.24 or newer
- **Git** (Catch2 is fetched automatically at configure time)

## Building

The build steps are identical on every platform; only the toolchain setup differs. From the repository root:

```
cmake -S . -B build -DADBCPP_BUILD_TESTS=ON -DADBCPP_BUILD_EXAMPLES=ON
cmake --build build --config Release
ctest --test-dir build --output-on-failure -C Release
```

The `--config Release` flag is used by multi-config generators (Visual Studio, Xcode) and ignored by single-config generators (Makefiles, Ninja).

### Linux

Prerequisites (Debian/Ubuntu):

```
sudo apt-get update
sudo apt-get install -y build-essential cmake git
```

Build:

```
cmake -S . -B build -DADBCPP_BUILD_TESTS=ON -DADBCPP_BUILD_EXAMPLES=ON
cmake --build build --config Release
ctest --test-dir build --output-on-failure -C Release
```

### macOS

Prerequisites: install the Xcode Command Line Tools (provides Clang) and CMake.

```
xcode-select --install
brew install cmake
```

Build:

```
cmake -S . -B build -DADBCPP_BUILD_TESTS=ON -DADBCPP_BUILD_EXAMPLES=ON
cmake --build build --config Release
ctest --test-dir build --output-on-failure -C Release
```

### Windows

Prerequisites:

- Visual Studio 2022 with the **Desktop development with C++** workload.
- CMake 3.24 or newer (bundled with Visual Studio, or from [cmake.org](https://cmake.org/download/)).

Build (from a Developer PowerShell for VS 2022, or any shell where CMake can find the Visual Studio generator):

```
cmake -S . -B build -DADBCPP_BUILD_TESTS=ON -DADBCPP_BUILD_EXAMPLES=ON
cmake --build build --config Release
ctest --test-dir build --output-on-failure -C Release
```

### Documentation (optional)

If [Doxygen](https://www.doxygen.nl/) is installed:

```
cmake -S . -B build -DADBCPP_BUILD_DOCS=ON
cmake --build build --target adbcpp_docs
```

## Project Layout

```
include/adbcpp/    Public headers (transport, protocol, session)
src/               Library sources
tests/             Catch2 unit tests
examples/          Sample project (examples + manual integration harness)
docs/              Design documents and Doxygen configuration
cmake/             CMake package configuration
```

## Learning

- [`LEARNING.md`](LEARNING.md) — a guided curriculum that uses this repository as a worked example, from byte channels to a full shell session and file listing
- [`docs/05-usage.md`](docs/05-usage.md) — copy-pasteable code examples for everything the library can do today
- [`docs/06-sync-protocol.md`](docs/06-sync-protocol.md) — how the `sync` service and `list` work, byte by byte

## Design Documents

- [`docs/01-objective.md`](docs/01-objective.md) — objective, technical requirements, versioning, and commit conventions
- [`docs/02-references.md`](docs/02-references.md) — reference material on the ADB protocol
- [`docs/03-roadmap.md`](docs/03-roadmap.md) — vertical-slice implementation roadmap
- [`docs/04-blockers.md`](docs/04-blockers.md) — significant blockers and how they were solved
- [`docs/05-usage.md`](docs/05-usage.md) — usage guide with copy-pasteable code examples
- [`docs/06-sync-protocol.md`](docs/06-sync-protocol.md) — the `sync` service wire format and how `list` is built on it

## References

- [ADB protocol documentation](https://github.com/cstyan/adbDocumentation)
- [Diving into ADB protocol internals (1/2)](https://www.synacktiv.com/en/node/1042)
- [Synacktiv/adb_client](https://github.com/Synacktiv/adb_client)

## License

Boost Software License 1.0. See [`LICENSE`](LICENSE).
