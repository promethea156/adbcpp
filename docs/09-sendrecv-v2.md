# `sendrecv_v2` and Compression

This document plans issue [#1](https://github.com/promethea156/adbcpp/issues/1): using
the `sendrecv_v2` forms of `RECV`/`SEND` with zstd so a transfer can be compressed.
LZ4 ([#35](https://github.com/promethea156/adbcpp/issues/35)) followed, then brotli
([#34](https://github.com/promethea156/adbcpp/issues/34)), so all three codecs are now
implemented. It is a plan, not a description of shipped code;
[`06-sync-protocol.md`](06-sync-protocol.md) describes the wire format that is shipped
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

## The codecs

All three codecs are implemented, in the order AOSP's `ResolveCompressionType`
prefers them:

| Codec | License | How easy to add | Speed / ratio |
| --- | --- | --- | --- |
| **Zstandard** | BSD-3-Clause | CMake and a single-file amalgamation | fast, best ratio |
| LZ4 (frame) | BSD-2-Clause | one `.c` + one `.h` | very fast, modest ratio |
| Brotli | MIT | many files, no single-file build | good ratio, slow encoder |

All three are permissive, so none adds a copyleft obligation. The project's
self-contained goal is about not depending on `adb`/the ADB server, not about zero
libraries: it already vendors libusb and mbedTLS. Zstd is the modern default and the
best ratio, and AOSP's own `ResolveCompressionType` prefers it, so it was implemented
first; LZ4 ([#35](https://github.com/promethea156/adbcpp/issues/35)) followed in the
same way, and brotli ([#34](https://github.com/promethea156/adbcpp/issues/34)) last,
bringing many sources because it has no single-file build.

Each codec has its own build option (`ADBCPP_BUILD_COMPRESSION` for zstd,
`ADBCPP_BUILD_LZ4` for lz4, `ADBCPP_BUILD_BROTLI` for brotli) and its own
`ADBCPP_HAS_*` define, so a build can turn one on without the others and the
banner names only the built ones. `Auto` picks the best one both sides have, in
adb's order.

The zstd usage is AOSP's exactly, from `compression_utils.h`:

- **Encoder**: `ZSTD_createCStream`, `ZSTD_CCtx_setParameter(..., ZSTD_c_compressionLevel, 1)`,
  then `ZSTD_compressStream2` with `ZSTD_e_continue`, and a final `ZSTD_e_end` flush.
  The output block is `SYNC_DATA_MAX` (64 KiB).
- **Decoder**: `ZSTD_createDStream`, then `ZSTD_decompressStream` into a `SYNC_DATA_MAX`
  buffer, which is done when it returns 0.

The compression level is fixed at 1, like adb: the transfer is link-bound, so a faster,
smaller-ratio setting is the right trade.

The lz4 usage is AOSP's `Lz4Encoder`/`Lz4Decoder`: the frame API (`LZ4F_*`), with
independent blocks, because a block has no framing of its own. The brotli usage is
AOSP's `BrotliEncoder`/`BrotliDecoder`: `BrotliEncoderCompress` with quality 1 for a
complete stream per chunk, and `BrotliDecoderDecompressStream` incrementally across the
device's chunks.

## The dependencies

Zstd, lz4, and brotli are each fetched with `FetchContent` behind its own option
(`ADBCPP_BUILD_COMPRESSION`, `ADBCPP_BUILD_LZ4`, and `ADBCPP_BUILD_BROTLI`, all
default `ON`), like libusb behind `ADBCPP_BUILD_USB`:

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
effect. Brotli's `BUILD_SHARED_LIBS` option is handled the same way, with the
caller's value saved and restored around its `FetchContent_MakeAvailable`, because
brotli would otherwise force this project's shared/static choice on.

`<zstd.h>`, `<lz4frame.h>`, and the brotli headers are kept out of the public
headers by putting the codecs in `src/compression.{hpp,cpp}`, which no public header
includes, so no codec type reaches a consumer:

```cpp
// src/compression.hpp
namespace adbcpp
{
/// Compresses one sync chunk as a complete zstd frame.
Result<std::vector<std::byte>> compress_zstd(std::span<const std::byte> input);

/// Decompresses a zstd stream incrementally, across `DATA` chunks.
class ZstdDecoder
{
  public:
    Result<std::vector<std::byte>> decode(std::span<const std::byte> input);
};

// The lz4 and brotli forms are declared beside it.
}
```

`adbcpp` links the codec libraries **privately** and defines
`ADBCPP_HAS_COMPRESSION` plus one `ADBCPP_HAS_*` per built codec, so a consumer's
public headers never see a codec. A private dependency of a static library still
propagates to the final link, so each codec joins the install/export set like
mbedcrypto (see `CMakeLists.txt`).

The codec is compiled into the core rather than into a separate target, because a
separate `adbcpp-compression` target would report failures through `Result`/`Error` and
therefore link `adbcpp::adbcpp`, while the core would link it back to call it — a link
cycle. A separate target would only be needed if the codec were part of the public API,
as `adbcpp::crypto` and `adbcpp::usb` are.

When every codec option is off, `src/compression.cpp` is not built, `pull`/`push`
always use v1, and the banner does not advertise `sendrecv_v2`. The core compiles either
way, because `src/sync.cpp` guards the v2 path with `#if defined(ADBCPP_HAS_COMPRESSION)`.
Each codec can be turned off on its own; the banner then drops only that name.

## The public API

Compression is opt-in and per call, matching `ShellProtocol`:

```cpp
// include/adbcpp/sync.hpp
enum class SyncCompression
{
    /// The best codec both sides have, in adb's order (zstd, then lz4, then
    /// brotli), and v1 otherwise. This is the default.
    Auto,
    /// The v1 `RECV`/`SEND` forms, with no compression.
    None,
    /// The v2 forms with zstd. The device must have advertised
    /// `sendrecv_v2_zstd`.
    Zstd,
    /// The v2 forms with lz4. The device must have advertised
    /// `sendrecv_v2_lz4`.
    Lz4,
    /// The v2 forms with brotli. The device must have advertised
    /// `sendrecv_v2_brotli`.
    Brotli
};

Status ADBCPP_API pull(Connection &connection, std::string_view remote_path,
                        const std::filesystem::path &local_path,
                        SyncCompression compression = SyncCompression::Auto);

Status ADBCPP_API push(Connection &connection, const std::filesystem::path &local_path,
                        std::string_view remote_path,
                        SyncCompression compression = SyncCompression::Auto);
```

`Auto` chooses v2 with the best codec both sides can, so a caller who does not care
gets the best available, and one who wants the v1 form passes `None`. A caller who
passes `Zstd`, `Lz4`, or `Brotli` to a device that did not advertise it is an
`InvalidArgument`, not a silent fallback, so the caller is not misled.

## The banner

The host banner advertises only what is implemented, so `kSystemIdentity` gains
`sendrecv_v2` and each built codec, and no others:

```cpp
// connection.hpp
// `sendrecv_v2` is appended when any codec is built, and each codec name
// (`sendrecv_v2_zstd`, `sendrecv_v2_lz4`, `sendrecv_v2_brotli`) only when that
// codec is built.
inline constexpr std::string_view kSystemIdentity = "host::features=shell_v2,stat_v2,ls_v2";
inline constexpr std::string_view kSendRecvV2Feature = ",sendrecv_v2";
inline constexpr std::string_view kSendRecvV2ZstdFeature = ",sendrecv_v2_zstd";
inline constexpr std::string_view kSendRecvV2Lz4Feature = ",sendrecv_v2_lz4";
inline constexpr std::string_view kSendRecvV2BrotliFeature = ",sendrecv_v2_brotli";
```

The device resets its own feature set from this list (blocker 6), so a name that is not
implemented is a promise the peer may act on; this is exactly blocker 32.

## Negotiation

`pull`/`push` choose the form as adb does:

1. If no codec is built, or the device did not advertise `sendrecv_v2`, use v1.
2. Otherwise, if the device advertised `sendrecv_v2_zstd`, use v2 with `kSyncFlagZstd`.
3. Otherwise, if it advertised `sendrecv_v2_lz4`, use v2 with `kSyncFlagLz4`.
4. Otherwise, if it advertised `sendrecv_v2_brotli`, use v2 with `kSyncFlagBrotli`.
5. Otherwise, use v1, because no codec both sides support is implemented.

Step 5 is a deliberate simplification of AOSP, which would use v2 with `flags = 0`. That
adds the setup packet for no gain, so v1 is used instead. This is recorded here.

## The implementation

`src/sync.cpp` gains, alongside the v1 helpers:

- `kRecvV2`, `kSendV2`, `kSyncFlagBrotli`, `kSyncFlagLz4`, `kSyncFlagZstd`.
- `write_recv_v2(stream, path, flags)` and `write_send_v2(stream, path, mode, flags)`, each
  one write of the path request and the setup packet, like AOSP's
  `SendRecv2`/`SendSend2`. The flags select the codec.
- `choose_codec`, which returns the best codec both sides have, or `nullopt` for v1.
- `pull_v2` and `push_v2`, which compress or decompress each `DATA` chunk with the chosen
  codec and otherwise follow the v1 loops. `DONE` and `FAIL` are unchanged.
- `pull`/`push` dispatch on the negotiated form before they open the stream.

The v1 `pull`/`push` bodies are otherwise unchanged, so a device without the feature keeps
working.

## The tests

- `tests/sync_test.cpp`: a mock device whose `RECV` v2 reply is a compressed `DATA`
  chunk and whose `DONE` ends it, so `pull` decompresses it; the same for `SEND` v2, so
  `push` compresses it and the device's `OKAY` is read; the v1 fallback when the
  device does not advertise the feature; and the codec order when a device advertises more
  than one.
- A round trip in one test: `push` compresses a buffer, the test decompresses the bytes the
  mock received, and `pull` decompresses what the mock sends, so the two halves agree.
- `tests/device_test.cpp`: a file round trip with each codec against the real device, checking
  the contents match, and one with `None` to prove v1 still works.

## The steps

1. **CMake and the codec.** Add `ADBCPP_BUILD_COMPRESSION`, the FetchContent, and
   `src/compression.{hpp,cpp}`, linked privately to the core.
2. **The codec.** Implement `compress_zstd` and `ZstdDecoder::decode` with the AOSP calls,
   and a unit test that round-trips a compressible buffer, one that does not compress, and one
   whose frame is split across chunks.
3. **The API and the banner.** Add `SyncCompression`, the two parameters, and the banner
   names behind `ADBCPP_HAS_COMPRESSION`.
4. **The v2 wire format.** Add the ids, the setup writers, and `pull_v2`/`push_v2` with the
   zstd codec, dispatching from `pull`/`push`.
5. **The tests.** Add the mock v2 tests and the device round trip.
6. **The docs.** Extend [`06-sync-protocol.md`](06-sync-protocol.md) with the v2 section,
   and update [`05-usage.md`](05-usage.md), [`03-roadmap.md`](03-roadmap.md) (mark #1 done),
   blocker 32's note, [`README.md`](../README.md), and [`CHANGELOG.md`](../CHANGELOG.md).

Steps 1-6 were followed for zstd, then repeated for lz4
([#35](https://github.com/promethea156/adbcpp/issues/35)) and brotli
([#34](https://github.com/promethea156/adbcpp/issues/34)), each behind its own option.

## The risks

- **The compressed chunk can be larger than the input.** A codec on incompressible data adds a
  header, so `size` can exceed the uncompressed chunk. The `SYNC_DATA_MAX` bound is on the
  uncompressed chunk, so the decoder's output buffer is `SYNC_DATA_MAX`; the compressed size
  is bounded by the codec's own bound instead, which is what the encoder cannot
  exceed and what `pull` checks the chunk size against. This must be
  tested, not assumed.
- **The device must advertise the feature.** The attached device does; an older one may not, so
  the v1 fallback must stay correct and tested.
- **The gain may be small.** An APK is already compressed, so the feature may not pay for
  itself there; this is a caller's choice, and `None` keeps the v1 form.

## Acceptance

`pull` and `push` use the v2 forms with the best codec both sides have against a device that
advertises it, the bytes on the wire are compressed, the transferred file is identical, and the v1
forms still work for a device without the feature and for a caller who passes `None`.
