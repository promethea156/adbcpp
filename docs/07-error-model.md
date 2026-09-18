# Error Model

`adbcpp` does not throw for its own failures. Every operation that can fail returns a
`Result<T>` carrying the reason, and the caller decides what to do with it. Exceptions are
caught only at the boundary where a third-party library can raise one, and are converted there
into the same `Result`.

This is a change from the first five slices, which threw `std::runtime_error` from every layer.
The rule below is what replaces them, and it is applied to the whole library, not only to new code.

## The rule

Every operation is in one of two cases, and the shape of its result follows the case.

1. **The device answered.** The answer is the return value. For a command that is its output and
   exit code; for a question it is the answer; for an operation with nothing to report it is
   `void`.
2. **The operation could not be carried out.** The transport failed, the stream failed, the device
   did not answer as the protocol requires, or it refused the request with a reason. That is an
   `Error`, returned as the unexpected value of the `Result`.

The distinction is not "severe versus mild", it is **who is being reported on**. `stat` of a path
that does not exist is case 1: the device answered, and the answer is "there is nothing there".
A sync `FAIL` is case 2: the daemon refused to carry the request out, and said why.

## The types

```cpp
// include/adbcpp/error.hpp
#include <tl/expected.hpp>

namespace adbcpp
{

/// What went wrong, so a caller can branch without matching on a message.
enum class ErrorCode
{
    /// The caller passed something the library cannot use: a path past the sync limit,
    /// a local file that is not a regular file, a write past the negotiated maximum.
    InvalidArgument,
    /// The link to the device failed: libusb reported an error, no matching device
    /// was found, or a transfer was short.
    Transport,
    /// The device did not answer as the protocol requires: an unexpected frame, an
    /// unexpected end of the stream, or an oversized name or chunk.
    Protocol,
    /// The device refused the request and gave a reason, which is the message. This is
    /// a sync `FAIL`.
    Device,
    /// The ADB key could not be found, read, generated, or used to sign.
    Crypto,
    /// A local file could not be read or written.
    Io
};

/// A failure, with the reason a human can read.
struct Error
{
    ErrorCode code = ErrorCode::Protocol;
    std::string message;
};

/// The outcome of an operation: a value, or the reason there is none.
template <typename T>
using Result = tl::expected<T, Error>;

/// The outcome of an operation with nothing to report.
using Status = tl::expected<void, Error>;

} // namespace adbcpp
```

`Result` and `Status` are aliases rather than new types, so the whole of `tl::expected`'s
interface is available: `has_value()`, `operator*`, `operator->`, `value_or`, `and_then`, `map`,
`or_else`. The alias exists so that the public API does not depend on the spelling of the library
that provides it: moving to `std::expected` in C++23 is then a change to `error.hpp` alone.

`Error::message` does **not** repeat the library's name, because the type already carries it. A
caller prints `error: <message>`.

## Constructors become factories

A constructor cannot return a `Result`, so every type whose construction can fail gets a named factory
and a private constructor. The factory is a static member function so that the name reads as the operation
it performs.

| Type | Before | After |
| --- | --- | --- |
| `UsbTransport` | `UsbTransport transport(id)` | `UsbTransport::open(id)` → `Result<UsbTransport>` |
| `Connection` | `Connection connection(transport, key, signer)` | `Connection::connect(transport, key, signer)` → `Result<Connection>` |
| `Stream` | `Stream stream(connection, service)` | `Stream::open(connection, service)` → `Result<Stream>` |
| `Key` | `Key::load_or_generate()` | → `Result<Key>` |

`UsbTransport::open` also takes the transfer budget, which `set_transfer_budget` used to set
afterwards, so a transport is fully configured before it exists. `Connection` and `Stream` need a move
constructor to be returned by value; `Stream`'s move constructor marks the moved-from object closed, so
it does not send a second `CLOSE` when it is destroyed.

`UsbTransport::is_present` returns `Result<bool>` rather than `bool`. It currently returns `false`
when libusb itself fails to initialize, which is indistinguishable from "no device is attached".

## The device's answer is a result

`run`, `install`, and `launch` return `Result<CommandResult>`; the command worked if
`CommandResult::success`, which the library sets per command because the device reports success
differently:

| Function | `success` is | Why |
| --- | --- | --- |
| `run` | `exit_code == 0` | The exit code is the whole answer. |
| `launch` | `exit_code == 0` | `am start` exits nonzero and prints `Error:` when the activity cannot be started. |
| `install`, `uninstall` | `exit_code == 0` **and** the output says `Success` | `pm` reports a rejection in its output, so the exit code alone is not enough. |

`PackageResult` is a `CommandResult` that adds `failure_reason()`, which extracts the reason from
`Failure [REASON]`; the two commands report a rejection differently (blocker 25). `CommandResult`
carries `output`, `exit_code`, and `success`, so the shape is defined once.

`close` returns `Status` rather than a `CommandResult`: `am force-stop` cannot report a failure, so
there is nothing for the caller to inspect. The device test still asserts that it succeeds.

## `std::optional` where absence is an answer

`std::optional` is used where "nothing" is a legitimate answer rather than a failure, so it is nested
inside the `Result`:

- `stat(connection, path)` → `Result<std::optional<FileStat>>`. The device reports a missing path inside
  its answer, so "nothing there" is `Result` with an empty `optional`, while a transport failure is
  `Result` with an `Error`.
- `is_running(connection, package)` → `Result<bool>`. `pidof` exits nonzero for a package that is not
  running, so "not running" is a definite `false`, not an absence.

## The API before and after

| Before | After |
| --- | --- |
| `Transport::read` → `std::size_t` | → `Result<std::size_t>` (0 still means end of stream) |
| `Transport::write` | → `Status` |
| `Transport::close` | → `void` (unchanged; it cannot fail) |
| `Session::send` | → `Status` |
| `Session::receive` → `Frame` | → `Result<Frame>` |
| `Stream::write` | → `Status` |
| `Stream::read` | → `Status` |
| `Stream::read_all` → `std::vector<std::byte>` | → `Result<std::vector<std::byte>>` |
| `Connection::Signer` | → `std::function<Result<std::vector<std::byte>>(std::span<const std::byte>)>` |
| `Message::data_length_of` | → `Result<std::uint32_t>` |
| `list` → `std::vector<DirEntry>` | → `Result<std::vector<DirEntry>>` |
| `pull`, `push` → `void` | → `Status` |
| `stat` → `std::optional<FileStat>` | → `Result<std::optional<FileStat>>` |
| `run` → `CommandResult` | → `Result<CommandResult>` |
| `install`, `uninstall` → `PackageResult` | → `Result<PackageResult>` |
| `Key::fingerprint` → `std::string` | → `Result<std::string>` |

`Key::sign` returns `Result<std::vector<std::byte>>` because mbedTLS can refuse to sign, and it is
the `Signer` callback's type, so `Connection::connect` propagates it.

## Third-party boundaries

Exceptions are caught where a third-party library can raise one, and nowhere else. The catch is at the
boundary, and what it catches becomes an `Error`.

| Source | How it is handled |
| --- | --- |
| `std::filesystem` | The `error_code` overloads are used, so nothing throws. `std::filesystem::path`'s constructor from a narrow string can throw `std::filesystem::filesystem_error` for an encoding error on Windows, so the paths that come from a device string are built inside a catch. |
| `std::ifstream` / `std::ofstream` | They do not throw; a failed open is tested with `operator bool`. |
| mbedTLS | A C API that returns codes. No catch is needed. |
| libusb | A C API that returns codes. No catch is needed. |
| `tl::expected` | `.value()` and `.error()` assert on the wrong alternative. The library never calls them without checking, so nothing is thrown; `operator*`, `operator->`, and `value_or` are used instead. |
| `std::bad_alloc` | Not caught. It comes from the allocator rather than the library, and there is no way to report it without allocating, so it is documented here instead of handled. |

## The dependency

`tl::expected` is header-only, licensed CC0-1.0, and therefore carries no obligations; the project's
BSL-1.0 and the "no third-party license obligations" goal (blocker 1) are unaffected. It is acquired
with `FetchContent` in the root `CMakeLists.txt`, before `add_library(adbcpp)`, because the core library
now depends on it:

```cmake
set(EXPECTED_BUILD_TESTS OFF)   # otherwise it fetches its own Catch2 v2
set(EXPECTED_BUILD_PACKAGE OFF) # otherwise it adds install and packaging rules
FetchContent_Declare(tl_expected
  GIT_REPOSITORY https://github.com/TartanLlama/expected.git
  GIT_TAG v1.3.1
)
FetchContent_MakeAvailable(tl_expected)
```

The two options have to be set before `FetchContent_MakeAvailable`, for the same reason
`LIBUSB_ENABLE_UDEV` and `MBEDTLS_FATAL_WARNINGS` do (blockers 20 and 22). `adbcpp` links
`tl::expected` **publicly**, because `<tl/expected.hpp>` appears in the public `error.hpp`, and the
install rules export it so a `find_package` consumer gets it too.

The core target is no longer dependency-free once this lands, so the "Extra dependency" column for
`adbcpp::adbcpp` in [`05-usage.md`](05-usage.md) gains `tl::expected`, and the claim in
[`01-objective.md`](01-objective.md) is narrowed to the crypto and USB backends.

## The state of the conversion

The conversion is complete. Every layer returns a `Result`, from `Transport` and `Session` up through
`Stream`, the `sync` and shell services, `app`, and `crypto`; the tests and the examples are converted
with them, and nothing in the library calls `value()` or `error()` without checking first. `format-check` is
clean, the 70 unit tests pass, and the device test passes against the test device. Slice 6 was the first work
that was written in the new shape rather than converted to it.
