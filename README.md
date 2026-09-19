# adbcpp

A small, self-contained **ADB (Android Debug Bridge) client, as a C++20 library**.

`adbcpp` talks to an Android device directly, over USB or TCP, with **no `adb.exe`, no server on port 5037, and no external binary**. Embed it in a C++ program and it runs shell commands, lists and transfers files, installs apps, and starts and stops them.

> **Status: 1.0.0.** The [initial scope](docs/01-objective.md#initial-scope) is complete, and every fallible operation returns a `Result<T>` instead of throwing. See [`CHANGELOG.md`](CHANGELOG.md) and [`docs/03-roadmap.md`](docs/03-roadmap.md).

## Platform support

| Platform | Status | Notes |
|----------|--------|-------|
| Windows  | ![Windows: tested](https://img.shields.io/badge/Windows-tested-brightgreen) | Developed and exercised here. |
| Linux    | ![Linux: untested](https://img.shields.io/badge/Linux-untested-yellow) | Expected to work; not yet verified. |
| macOS    | ![macOS: untested](https://img.shields.io/badge/macOS-untested-yellow) | Expected to work; not yet verified. |

**`Windows` is the only platform this project has been built and run on so far.** The
Linux and macOS instructions below are written from the toolchain and standard-library
APIs the code targets, but no one has confirmed them on a real machine yet — expect
rough edges. If you have either, please build it and
[report the result](https://github.com/promethea156/adbcpp/issues/new?template=platform_verification.yml);
a green run is just as useful as a red one, and see [Contributing](#contributing).

## What it can do

- **Shell**: run a command and read its combined output and exit code.
- **Files**: list a directory, `stat` a path, and pull or push a file.
- **Apps**: install and uninstall a package, launch it, check whether it is running, and close it.
- **Connect**: reach a device over USB, or over TCP to a `tcpip` device or an emulator, and pick one by its USB serial. The TCP path is verified against a `tcpip` device; an emulator's listener speaks the same plaintext protocol and is expected to work.

## Build it

You need a **C++20 compiler**, **CMake 3.24 or newer**, and **Git** (the test framework is fetched automatically at configure time). The build is the same everywhere; only the toolchain setup differs. Only **Windows** is known to work so far — see [Platform support](#platform-support).

Every dependency is fetched by CMake, so there is nothing else to install. The compiler, USB driver, and device steps for each platform are in [`docs/08-platform-setup.md`](docs/08-platform-setup.md).

From the repository root:

```
cmake -S . -B build -DADBCPP_BUILD_TESTS=ON -DADBCPP_BUILD_EXAMPLES=ON
cmake --build build --config Release
ctest --test-dir build --output-on-failure -C Release
```

The `--config Release` flag is used by multi-config generators (Visual Studio, Xcode) and ignored by single-config generators (Makefiles, Ninja). The tests that need a device skip themselves when none is attached, so the build and the suite should pass on any machine.

<details>
<summary><b>Linux</b> — untested</summary>

Install the toolchain:

```
sudo apt-get update
sudo apt-get install -y build-essential cmake git
```

Then run the three commands above.
</details>

<details>
<summary><b>macOS</b> — untested</summary>

Install the Xcode Command Line Tools (Clang) and CMake:

```
xcode-select --install
brew install cmake
```

Then run the three commands above.
</details>

<details>
<summary><b>Windows</b> — tested</summary>

Install **Visual Studio 2022** with the *Desktop development with C++* workload, and CMake 3.24 or newer. Then run the three commands above from a **Developer PowerShell for VS 2022**.
</details>

Optional API documentation, if [Doxygen](https://www.doxygen.nl/) is installed:

```
cmake -S . -B build -DADBCPP_BUILD_DOCS=ON
cmake --build build --target adbcpp_docs
```

## Take the guided tour

The fastest way to learn the library is to run
[`examples/demo/main.cpp`](examples/demo/main.cpp) and read it as it runs. It is one
file, and every step is commented with what the call does on the device and which
protocol it speaks, so the source is the walkthrough. It:

1. detects an attached device and connects to it, with the CNXN/AUTH handshake;
2. runs a shell command and reads its output and exit code;
3. lists a directory over the `sync` service;
4. pushes a file, stats it, and pulls it back;
5. puts the device to sleep;
6. installs an app, uninstalling an old copy first;
7. wakes the device;
8. launches the app;
9. checks that it stays running for ten seconds;
10. closes it.

To run it, you need a device with **USB debugging enabled**, and an APK to install
(the package and its split APKs, base first). It uninstalls the package first, so it
loses that package's data.

```
# adb holds the device's USB interface, so stop its server first
adb kill-server

build/examples/Release/adbcpp_demo_example <package> <apk> [<split-apk>...] [--serial <serial>]
```

The binary is under `build/examples/Release/` for a multi-config generator (Visual
Studio, Xcode) and `build/examples/` for a single-config one (Makefiles, Ninja).

With no `--serial`, it prints the attached devices and uses the first. When it finishes,
open the source and read it next to the output: each `step(...)` in the source is one of
the ten steps above.

**No APK handy?** Pull one off the device first:

```
adb shell pm path <package>     # prints the APK path(s)
adb pull <path> app.apk
```

When you are ready for the theory behind what the tour did, [`LEARNING.md`](LEARNING.md) is
a module-by-module curriculum that uses this repository as the worked example, from byte
channels to a full shell session. The tour is its hands-on counterpart.

## Project Layout

```
include/adbcpp/         Public headers (transport, protocol, session, stream,
                        shell, sync, app)
include/adbcpp/crypto/  The ADB key pair, backed by mbedTLS
include/adbcpp/tcp/      The TCP transport, over the platform's sockets
include/adbcpp/usb/      The USB transport, backed by libusb
include/adbcpp/testing/  The in-memory transport used by the tests
src/                    Library sources, mirroring the public headers
tests/                  Catch2 unit tests and the device integration test
examples/               Runnable examples, including the guided tour in demo/
tools/                  Developer scripts (adb wrapper, transfer benchmark)
docs/                   Design documents and Doxygen configuration
cmake/                  CMake package configuration
```

## Where to go next

- [`examples/demo/main.cpp`](examples/demo/main.cpp) — the guided tour, step by step in its comments.
- [`LEARNING.md`](LEARNING.md) — the theory: a guided curriculum, module by module.
- [`docs/05-usage.md`](docs/05-usage.md) — copy-pasteable snippets for one feature at a time.
- [`docs/06-sync-protocol.md`](docs/06-sync-protocol.md) — how the `sync` service and file transfer work, byte by byte.
- [`docs/03-roadmap.md`](docs/03-roadmap.md) and the [open issues](https://github.com/promethea156/adbcpp/issues) — what is planned next.

## Design documents

- [`docs/01-objective.md`](docs/01-objective.md) — objective, technical requirements, versioning, and commit conventions
- [`docs/02-references.md`](docs/02-references.md) — reference material on the ADB protocol
- [`docs/03-roadmap.md`](docs/03-roadmap.md) — vertical-slice implementation roadmap
- [`docs/04-blockers.md`](docs/04-blockers.md) — significant blockers and how they were solved
- [`docs/05-usage.md`](docs/05-usage.md) — usage guide with copy-pasteable code examples
- [`docs/06-sync-protocol.md`](docs/06-sync-protocol.md) — the `sync` service wire format and how `list` is built on it
- [`docs/07-error-model.md`](docs/07-error-model.md) — why nothing throws, and what `Result<T>` carries instead
- [`docs/08-platform-setup.md`](docs/08-platform-setup.md) — per-platform compiler, dependency, USB driver, and device setup

## Contributing

Contributions are welcome, and the most useful ones often need no new code: verifying a
platform, reporting a failure, or fixing a document. See [`CONTRIBUTING.md`](CONTRIBUTING.md)
for how to build, test, and open a pull request, and the
[roadmap](docs/03-roadmap.md#future-improvements) for what is planned. Issues labelled
[`good first issue`](https://github.com/promethea156/adbcpp/labels/good%20first%20issue)
are a good place to start.

## References

- [ADB protocol documentation](https://github.com/cstyan/adbDocumentation)
- [Diving into ADB protocol internals (1/2)](https://www.synacktiv.com/en/node/1042)
- [Synacktiv/adb_client](https://github.com/Synacktiv/adb_client)

## License

Boost Software License 1.0. See [`LICENSE`](LICENSE).
