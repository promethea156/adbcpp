# `sendrecv_v2` and Compression

This document plans issue [#1](https://github.com/promethea156/adbcpp/issues/1): using
the `sendrecv_v2` forms of `RECV`/`SEND` with zstd so a transfer can be compressed.
LZ4 ([#35](https://github.com/promethea156/adbcpp/issues/35)) and brotli
([#34](https://github.com/promethea156/adbcpp/issues/34)) are follow-ups, so #1 is
zstd-only. It is a plan, not a description of shipped code;
[`06-sync-protocol.md`](06-sync-protocol.md) describes the v1 forms that are shipped
today.

## Why

`pull` and `push` always send the v1 `RECV`/`SEND` forms, so a transfer is never
compressed. Slice 9 removed the false `sendrecv_v2` claim from the host banner
(blocker 32), because the host advertised a feature it did not implement. This is the
other half of that decision: implement the v2 forms, then advertise them again, so the
claim is true.

The gain is real only when the link is the bottleneck and the data compresses. A text
file shrinks a lot; an APK is already a ZIP and barely shrinks. The cost is CPU and a
new third-party dependency, so the feature is opt-in.

## What the device supports

A probe of the attached device (`0x22D9:0x2769`) shows it advertises every codec:

```
sendrecv_v2: 1   sendrecv_v2_brotli: 1   sendrecv_v2_lz4: 1
sendrecv_v2_zstd: 1   sendrecv_v2_dry_run_send: 1
```

So any codec is on the table, and the choice is ours, not the device's.

## The wire format

The authoritative layouts are AOSP's `file_sync_protocol.h`, `client/file_sync_client.cpp`,
and `compression_utils.h`:

- `file_sync_protocol.h`: <https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/file_sync_protocol.h>
- `file_sync_client.cpp`: <https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/client/file_sync_client.cpp>
- `compression_utils.h`: <https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/compression_utils.h>

The two v2 request ids are the little-endian encoding of four ASCII characters, like
the v1 ids:

```c
#define ID_SEND_V2 MKID('S', 'N', 'D', '2')   // "SND2"
#define ID_RECV_V2 MKID('R', 'C', 'V', '2')   // "RCV2"

enum SyncFlag : uint32_t {
    kSyncFlagNone   = 0,
    kSyncFlagBrotli = 1,
    kSyncFlagLZ4    = 2,
    kSyncFlagZstd   = 4,
    kSyncFlagDryRun = 0x8000'0000U,
};

struct sync_send_v2 { uint32_t id; uint32_t mode; uint32_t flags; };  // 12 bytes
struct sync_recv_v2 { uint32_t id; uint32_t flags; };                // 8 bytes
```

The request is the v1 path request followed by one setup packet. AOSP sends it as a
single write:

- **`RECV` v2**: `SyncRequest{id=ID_RECV_V2, path_length}` + path + `sync_recv_v2{id=ID_RECV_V2, flags}`.
- **`SEND` v2**: `SyncRequest{id=ID_SEND_V2, path_length}` + path + `sync_send_v2{id=ID_SEND_V2, mode, flags}`.
  Unlike v1, the path carries no `,mode` spec; the mode is in the setup packet.

Only the `DATA` payload changes: its `sync_data{id, size}` header is unchanged, and the
`size` bytes are compressed. `DONE` and `FAIL` are unchanged, and `DONE`'s size is still
the modification time.

The codec is chosen by the setup packet's `flags`. With `flags = 0` the transfer is
byte-for-byte the v1 form apart from the setup packet, which is why a caller can force
v2 without compression.

## The codec: zstd

Zstandard is the choice, over LZ4 and brotli:

| Codec | License | How easy to add | Speed / ratio |
| --- | --- | --- | --- |
| **Zstandard** | BSD-3-Clause | CMake and a single-file amalgamation | fast, best ratio |
| LZ4 (frame) | BSD-2-Clause | one `.c` + one `.h` | very fast, modest ratio |
| Brotli | MIT | many files, no single-file build | good ratio, slow encoder |

All three are permissive, so none adds a copyleft obligation. The project's
self-contained goal is about not depending on `adb`/the ADB server, not about zero
libraries: it already vendors libusb and mbedTLS. Zstd is the modern default and the
best ratio, and AOSP's own `ResolveCompressionType` prefers it, so it is implemented
first. LZ4 ([#35](https://github.com/promethea156/adbcpp/issues/35)) and brotli
([#34](https://github.com/promethea156/adbcpp/issues/34)) are deferred; if either is added
later, only its feature name is advertised.

The zstd usage is AOSP's exactly, from `compression_utils.h`:

- **Encoder**: `ZSTD_createCStream`, `ZSTD_CCtx_setParameter(..., ZSTD_c_compressionLevel, 1)`,
  then `ZSTD_compressStream2` with `ZSTD_e_continue`, and a final `ZSTD_e_end` flush.
  The output block is `SYNC_DATA_MAX` (64 KiB).
- **Decoder**: `ZSTD_createDStream`, then `ZSTD_decompressStream` into a `SYNC_DATA_MAX`
  buffer, which is done when it returns 0.

The compression level is fixed at 1, like adb: the transfer is link-bound, so a faster,
smaller-ratio setting is the right trade.

## The dependency

Zstd is fetched with `FetchContent` behind `ADBCPP_BUILD_COMPRESSION` (default `ON`),
like libusb behind `ADBCPP_BUILD_USB`:

```cmake
set(ZSTD_BUILD_PROGRAMS OFF)
set(ZSTD_BUILD_TESTS OFF)
set(ZSTD_BUILD_SHARED OFF)
set(ZSTD_BUILD_STATIC ON)
FetchContent_Declare(zstd
  GIT_REPOSITORY https://github.com/facebook/zstd.git
  GIT_TAG v1.5.7
  SOURCE_SUBDIR build/cmake
)
FetchContent_MakeAvailable(zstd)
```

`ZSTD_BUILD_PROGRAMS` and `ZSTD_BUILD_TESTS` are set before
`FetchContent_MakeAvailable` for the same reason `LIBUSB_ENABLE_UDEV` and
`EXPECTED_BUILD_TESTS` are (blockers 20 and 22): an option set afterwards has no
effect.

A new `adbcpp-compression` target keeps `<zstd.h>` out of the public headers, exactly
like `adbcpp::usb` keeps `<libusb.h>` out:

```cpp
// include/adbcpp/compression.hpp
namespace adbcpp
{
/// Compresses `data` with zstd, or reports why it cannot.
Result<std::vector<std::byte>> ADBCPP_API compress_zstd(std::span<const std::byte> data);

/// Decompresses at most `uncompressed_size` bytes with zstd.
Result<std::vector<std::byte>> ADBCPP_API decompress_zstd(std::span<const std::byte> data,
                                                        std::size_t uncompressed_size);
}
```

The core links `adbcpp-compression` **privately**, so a consumer's public headers never
see zstd. The static target propagates its private dependency to the final link, so zstd
joins the install/export set like mbedcrypto (see `CMakeLists.txt`).

When `ADBCPP_BUILD_COMPRESSION=OFF`, the target is not built, `pull`/`push` always use
v1, and the banner does not advertise `sendrecv_v2`. The core compiles either way,
because `src/sync.cpp` guards the v2 path with `#if defined(ADBCPP_HAS_COMPRESSION)`.

## The public API

Compression is opt-in and per call, matching `ShellProtocol`:

```cpp
// include/adbcpp/sync.hpp
enum class SyncCompression
{
    /// zstd when the device advertised `sendrecv_v2_zstd` and the build has
    /// compression, and v1 otherwise. This is the default.
    Auto,
    /// The v1 `RECV`/`SEND` forms, with no compression.
    None,
    /// The v2 forms with zstd. The device must have advertised
    /// `sendrecv_v2_zstd`.
    Zstd
};

Status ADBCPP_API pull(Connection &connection, std::string_view remote_path,
                        const std::filesystem::path &local_path,
                        SyncCompression compression = SyncCompression::Auto);

Status ADBCPP_API push(Connection &connection, const std::filesystem::path &local_path,
                        std::string_view remote_path,
                        SyncCompression compression = SyncCompression::Auto);
```

`Auto` chooses v2 with zstd only when both sides can, so a caller who does not care
gets the best available, and one who wants the v1 form passes `None`. A caller who
passes `Zstd` to a device that did not advertise it is an `InvalidArgument`, not a
silent fallback, so the caller is not misled.

## The banner

The host banner advertises only what is implemented, so `kSystemIdentity` gains the two
names when the build has compression, and no others:

```cpp
// connection.hpp
#if defined(ADBCPP_HAS_COMPRESSION)
inline constexpr std::string_view kSystemIdentity =
    "host::features=shell_v2,stat_v2,ls_v2,sendrecv_v2,sendrecv_v2_zstd";
#else
inline constexpr std::string_view kSystemIdentity = "host::features=shell_v2,stat_v2,ls_v2";
#endif
```

The device resets its own feature set from this list (blocker 6), so a name that is not
implemented is a promise the peer may act on; this is exactly blocker 32.

## Negotiation

`pull`/`push` choose the form as adb does:

1. If the build has no compression, or the device did not advertise `sendrecv_v2`, use v1.
2. Otherwise, if the device advertised `sendrecv_v2_zstd`, use v2 with `kSyncFlagZstd`.
3. Otherwise, use v1, because no codec both sides support is implemented.

Step 3 is a deliberate simplification of AOSP, which would use v2 with `flags = 0`. That
adds the setup packet for no gain, so v1 is used instead. This is recorded here.

## The implementation

`src/sync.cpp` gains, alongside the v1 helpers:

- `kSendV2`, `kRecvV2`, `kSyncFlagZstd`.
- `write_send_v2(stream, spec, mode, flags)` and `write_recv_v2(stream, path, flags)`, each
  one write of the path request and the setup packet, like AOSP's `SendSend2`/`SendRecv2`.
- `pull_v2` and `push_v2`, which compress or decompress each `DATA` chunk with zstd and
  otherwise follow the v1 loops. `DONE` and `FAIL` are unchanged.
- `run_pull`/`run_push` dispatch on the negotiated form.

The v1 `pull`/`push` are unchanged, so a device without the feature keeps working.

## The tests

- `tests/sync_test.cpp`: a mock device whose `RECV` v2 reply is a zstd-compressed `DATA`
  chunk and whose `DONE` ends it, so `pull` decompresses it; the same for `SEND` v2, so
  `push` compresses it and the device's `OKAY` is read; and the v1 fallback when the
  device does not advertise the feature.
- A round trip in one test: `push` compresses a buffer, the test decompresses the bytes the
  mock received, and `pull` decompresses what the mock sends, so the two halves agree.
- `tests/device_test.cpp`: a file round trip with `SyncCompression::Zstd` against the real
  device, checking the contents match, and one with `None` to prove v1 still works.

## The steps

1. **CMake and the target.** Add `ADBCPP_BUILD_COMPRESSION`, the FetchContent, and
   `adbcpp-compression`; add `include/adbcpp/compression.hpp` and `src/compression.cpp`.
2. **The codec.** Implement `compress_zstd`/`decompress_zstd` with the AOSP calls, and a
   unit test that round-trips a compressible buffer and one that compresses to more than it
   started with.
3. **The API and the banner.** Add `SyncCompression`, the two parameters, and the banner
   names behind `ADBCPP_HAS_COMPRESSION`.
4. **The v2 wire format.** Add the ids, the setup writers, and `pull_v2`/`push_v2` with the
   zstd codec, dispatching from `pull`/`push`.
5. **The tests.** Add the mock v2 tests and the device round trip.
6. **The docs.** Extend [`06-sync-protocol.md`](06-sync-protocol.md) with the v2 section,
   and update [`05-usage.md`](05-usage.md), [`03-roadmap.md`](03-roadmap.md) (mark #1 done),
   blocker 32's note, [`README.md`](../README.md), and [`CHANGELOG.md`](../CHANGELOG.md).

## The risks

- **The compressed chunk can be larger than the input.** Zstd on incompressible data adds a
  header, so `size` can exceed the uncompressed chunk. The `SYNC_DATA_MAX` bound is on the
  compressed size, so a chunk at the cap is still framed; the encoder must be fed at most
  `SYNC_DATA_MAX` and its output is at most `SYNC_DATA_MAX` for level 1. This must be
  tested, not assumed.
- **The device must advertise the feature.** The attached device does; an older one may not, so
  the v1 fallback must stay correct and tested.
- **The gain may be small.** An APK is already compressed, so the feature may not pay for
  itself there; this is a caller's choice, and `None` keeps the v1 form.

## Acceptance

`pull` and `push` use the v2 forms with zstd against a device that advertises them, the bytes
on the wire are compressed, the transferred file is identical, and the v1 forms still work for a
device without the feature and for a caller who passes `None`.
