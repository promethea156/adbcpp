# Platform Setup

What each platform needs to build and run `adbcpp`, and which of it is a real dependency
versus something the build fetches for you.

**Only Windows has been built and run so far.** The Linux and macOS steps are written from
the toolchain and the code's targets, but nobody has confirmed them on real hardware. If you
try one, please [report the result](../CONTRIBUTING.md#verify-a-platform).

## What you actually install

Almost nothing. Every third-party dependency is fetched and built by CMake at configure time, so
the only things to install by hand are a C++20 compiler, CMake, and Git:

| Dependency | Version | License | Where it goes | Fetched by |
| --- | --- | --- | --- | --- |
| `tl::expected` | v1.3.1 | CC0-1.0 | Core, public headers | `CMakeLists.txt` |
| mbedTLS | v3.6.2 | Apache-2.0 | `adbcpp::crypto` | `src/crypto/CMakeLists.txt` |
| libusb | v1.0.30-0 | LGPL-2.1-or-later | `adbcpp::usb` | `src/usb/CMakeLists.txt` |
| Catch2 | v3.7.1 | BSL-1.0 | Tests only | `tests/CMakeLists.txt` |

`libusb` is fetched through the community `libusb/libusb-cmake` build and is deliberately
linked **dynamically**, because its LGPL-2.1-or-later license must not be statically linked into
the BSL-1.0 core ([blocker 1](04-blockers.md#1-usb-backend-libusb-dynamically-linked)). The
core adds only `tl::expected`, a header-only CC0-1.0 library. A consumer that only needs TCP
never pulls `libusb` in.

There is no Android SDK, no NDK, no Java, and no `adb` server in the build. `adb` is only useful
for preparing a device (see [Prepare a device](#prepare-a-device)) and as a reference capture.

## Common requirements

- A **C++20 compiler**. CI builds with the `ubuntu-latest`, `windows-latest`, and `macos-latest`
  images, so a current GCC, Clang/AppleClang, or MSVC 19.3x (Visual Studio 2022) works.
- **CMake 3.24 or newer** (the `cmake_minimum_required` in [`CMakeLists.txt`](../CMakeLists.txt)).
- **Git**, used by `FetchContent` to clone the dependencies above.
- **Network access on the first configure.** Later configures reuse `build/_deps`, and
  [Offline builds](#offline-and-reproducible-builds) shows how to pin them.

A single-config generator (Makefiles, Ninja) defaults to `Release`
([`CMakeLists.txt`](../CMakeLists.txt)); a multi-config one (Visual Studio, Xcode) needs
`--config Release` on every build and test command.

## Windows

Install **Visual Studio 2022** with the *Desktop development with C++* workload, and CMake 3.24+.
Run the build from a **Developer PowerShell for VS 2022** so `cl.exe` is on `PATH`:

```powershell
cmake -S . -B build -DADBCPP_BUILD_TESTS=ON -DADBCPP_BUILD_EXAMPLES=ON
cmake --build build --config Release
ctest --test-dir build -C Release -E "^device$" --output-on-failure
```

- **USB device access** needs the device's ADB interface bound to the **WinUSB** driver. Google's
  `adb` already uses WinUSB on Windows, so on most phones this is done; otherwise bind it with
  [Zadig](https://zadig.akeo.ie/) (select the ADB interface, then WinUSB). WinUSB admits **one
  handle per device**, so a device cannot be opened twice, and `adb` must be stopped first
  ([blocker 28](04-blockers.md#28-winusb-allows-one-handle-per-device-so-one-device-cannot-be-opened-twice)).
- **DLLs at runtime.** The examples and tests copy `$<TARGET_RUNTIME_DLLS:...>` (including the
  libusb DLL) next to the executable on a `POST_BUILD` step, so running from the build tree needs
  no `PATH` change. For an installed copy, the DLL lands in `<prefix>/bin`; add it to `PATH`
  ([`05-usage.md`](05-usage.md)).
- **clang-format** ships with the VS LLVM component; `cmake/Format.cmake` finds it under the
  Visual Studio install automatically.

## Linux

Install the toolchain:

```sh
sudo apt-get update
sudo apt-get install -y build-essential cmake git
```

Then run the same three commands as above. Two Linux specifics:

- **No `libudev` headers are needed at build time.** libusb's udev backend is turned off
  (`LIBUSB_ENABLE_UDEV OFF`), so it builds against its sysfs/netlink backend with no system
  package ([blocker 20](04-blockers.md#20-libusbs-udev-backend-needs-libudevh-on-linux)).
- **USB device access at runtime needs permission.** The ADB device node is root-owned by default;
  either run as root or add a udev rule. The `adb` package ships `51-android.rules`, and a minimal
  rule is:

  ```sh
  # /etc/udev/rules.d/51-android.rules
  SUBSYSTEM=="usb", ATTR{idVendor}=="18d1", MODE="0666", GROUP="plugdev"
  ```

  Reload it with `sudo udevadm control --reload-rules && sudo udevadm trigger`, and make sure
  your user is in `plugdev` (`sudo usermod -aG plugdev $USER`, then log back in). A device
  with a vendor other than Google needs that vendor's USB id; `lsusb` prints it. TCP needs none
  of this.

The shared library is found through the build-tree `RPATH`, so unlike Windows there is no DLL to copy.

## macOS

Install the Xcode Command Line Tools (Clang) and CMake:

```sh
xcode-select --install
brew install cmake
```

Then run the same three commands. There is **no driver to install** — libusb talks to IOKit
directly. The build already works around a newer AppleClang warning inside mbedTLS by turning its
fatal warnings off ([blocker 22](04-blockers.md#22-mbedtls-compiles-with--werror-and-a-newer-clang-warns)),
and around the empty `TARGET_RUNTIME_DLLS` on a non-DLL platform
([blocker 21](04-blockers.md#21-target_runtime_dlls-is-empty-on-non-dll-platforms)).

## Prepare a device

A device is only needed for `adbcpp_device_tests`, `adbcpp_demo_example`,
`adbcpp_demo_multi_example`, `adbcpp_usb_example`, and `adbcpp_multi_example`;
everything else runs against the mock or a TCP loopback.

1. Enable **USB debugging** in Developer Options and connect the device by USB.
2. Approve the on-device **RSA fingerprint prompt** the first time. The key is
   `~/.android/adbkey`, the same key `adb` uses, so an already-authorized machine is not prompted.
3. **Stop `adb` before opening the device.** `adb` holds the USB interface, so the two cannot
   share it. In this repository, run `adb` through
   [`tools/invoke-adb.ps1`](../tools/invoke-adb.ps1) because `adb` otherwise holds the agent's
   console open (see [`AGENTS.md`](../AGENTS.md)).

For a device that is not on the other end of a cable, put it into TCP mode and connect to
`host:5555`:

```sh
adb tcpip 5555
# run adbcpp against <phone-ip>:5555
adb usb        # restore USB mode
```

An emulator's ADB listener is already on `localhost:5555`. TCP needs no libusb, so it can be built
with `-DADBCPP_BUILD_USB=OFF`.

The install/uninstall round trip in the device test is destructive and needs a disposable APK:

```powershell
$env:ADBCPP_TEST_APK = "path\to\app.apk"
$env:ADBCPP_TEST_PACKAGE = "com.example.app"
ctest --test-dir build -C Release -R "^device$" --output-on-failure
```

## Optional tools

- **Doxygen** for the API docs: configure with `-DADBCPP_BUILD_DOCS=ON` and build the
  `adbcpp_docs` target.
- **clang-format 19.1.1** for `format` / `format-check`; set `-DADBCPP_CLANG_FORMAT=<path>`
  if it is not on `PATH`.
- **USBPcap + Wireshark** to capture a real `adb` session for comparison, which is how several
  [blockers](04-blockers.md) were diagnosed.

## Offline and reproducible builds

`FetchContent` caches each clone under `build/_deps`; a later configure with the same build
directory does not re-clone. To build with no network at all, point each dependency at an existing
checkout:

```
cmake -S . -B build \
  -DFETCHCONTENT_SOURCE_DIR_TL_EXPECTED=<path> \
  -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=<path> \
  -DFETCHCONTENT_SOURCE_DIR_LIBUSB_CMAKE=<path> \
  -DFETCHCONTENT_SOURCE_DIR_CATCH2=<path>
```

`FETCHCONTENT_FULLY_DISCONNECTED=ON` uses only what is already in `build/_deps`. The pinned
tags (`v1.3.1`, `v3.6.2`, `v1.0.30-0`, `v3.7.1`) are what CI resolves, so a local build
matches it.

## Check the environment

The suite that needs no device should pass, and the device test should either pass or report a skip:

```sh
ctest --test-dir build -C Release -E "^device$" --output-on-failure
ctest --test-dir build -C Release -R "^device$" --output-on-failure   # optional, needs a device
```

`-E "^device$"` is anchored on purpose: an unanchored `-E device` also skips every test whose
name merely contains `device`, and the run still reports success. The device test exits with code 77
(a CTest skip) when no matching device is attached. When the build has no USB backend
(`-DADBCPP_BUILD_USB=OFF`), there is no `device` test to select, so `-R "^device$"` matches nothing.
