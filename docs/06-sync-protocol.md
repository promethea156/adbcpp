# The sync Service

This document explains the `sync` service end to end: why it is a second protocol,
what its bytes look like, and how `list` in [`src/sync.cpp`](../src/sync.cpp) is
built on top of `Stream`. It is the companion to Slice 2 in
[`03-roadmap.md`](03-roadmap.md).

The authoritative references are AOSP's `docs/dev/sync.md` and
`file_sync_protocol.h`, both linked from [`02-references.md`](02-references.md).

## Two protocols, one stream

An ADB `WRTE` payload is an opaque byte string. The **ADB protocol** says how bytes
are framed into messages (`CNXN`, `OPEN`, `WRTE`, `CLSE`, ...); a **service
protocol** gives those payload bytes meaning. `shell:` uses `shell_v2` packets;
`sync:` uses the format below.

So `sync` is not a new transport and not a new connection. It is a small binary
protocol that travels inside the payload of one stream:

```
Application
  list()                        <- src/sync.cpp        (this document)
    request / DENT framing
Stream (OPEN/OKAY/WRTE/CLSE)    <- src/stream.cpp
Session (24-byte ADB headers)   <- src/session.cpp
Transport (bytes)               <- src/usb/usb_transport.cpp
```

Everything below `list` existed before Slice 2. That is the payoff of the layering:
a new service changes only the top layer, and it can be tested with the same mock
transport as everything else.

## The request format

A sync request is a four-byte id, a four-byte little-endian length, and then that
many bytes of data:

```
 0               4               8
 +---------------+---------------+-----------------------+
 |      id       |    length     |    data ...           |
 +---------------+---------------+-----------------------+
```

The id is the little-endian encoding of four ASCII characters, exactly like an ADB
command, so `sync` reuses `protocol::make_command` (`src/sync.cpp:29`).

Requests:

| Constant   | ASCII  | Meaning                        |
| ---------- | ------ | ------------------------------ |
| `kListV1`  | `LIST` | list a directory               |
| `kListV2`  | `LIS2` | list a directory, v2 entries    |
| `kQuit`    | `QUIT` | leave sync mode                |

Responses:

| Constant   | ASCII  | Meaning                        |
| ---------- | ------ | ------------------------------ |
| `kDentV1`  | `DENT` | a directory entry              |
| `kDentV2`  | `DNT2` | a directory entry, v2          |
| `kDone`    | `DONE` | the listing is over             |
| `kFail`    | `FAIL` | the request was rejected       |

For `LIST`/`LIS2` the `data` is the path, **not** null-terminated, and it must be
at most 1024 bytes. This is the first difference from the ADB protocol: an `OPEN`
payload is null-terminated, a sync path is not.

### Worked example: listing `/sdcard`

The path `/sdcard` is 7 bytes, so the v1 request is:

```
4c 49 53 54   07 00 00 00   2f 73 64 63 61 72 64
L  I  S  T    7 (LE)         /  s  d  c  a  r  d
```

When the device advertises `ls_v2`, the id is `LIS2` instead:

```
4c 49 53 32   07 00 00 00   2f 73 64 63 61 72 64
L  I  S  2    7 (LE)         /  s  d  c  a  r  d
```

## The DENT response

Every response also starts with a four-byte id. A directory entry then continues
with a fixed-size body and the name.

The v1 body is 16 bytes:

| Body offset | Size | Field     |
| ----------- | ---- | --------- |
| 0           | 4    | `mode`    |
| 4           | 4    | `size`    |
| 8           | 4    | `mtime`   |
| 12          | 4    | `namelen` |
| 16          | `namelen` | `name` |

The v2 body is 72 bytes and adds a per-entry error and the full `lstat`
metadata:

| Body offset | Size | Field     |
| ----------- | ---- | --------- |
| 0           | 4    | `error`   |
| 4           | 8    | `dev`     |
| 12          | 8    | `ino`     |
| 20          | 4    | `mode`    |
| 24          | 4    | `nlink`   |
| 28          | 4    | `uid`     |
| 32          | 4    | `gid`     |
| 36          | 8    | `size`    |
| 44          | 8    | `atime`   |
| 52          | 8    | `mtime`   |
| 60          | 8    | `ctime`   |
| 68          | 4    | `namelen` |
| 72          | `namelen` | `name` |

Two consequences of the v2 layout:

- `size` is 64-bit, so a file larger than 4 GiB is reported correctly; the v1
  form truncates it to 32 bits.
- `error` is per entry. A failed `lstat` still sends the name, so the stream stays
  in sync and `list` can skip just that entry instead of losing the rest of the
  listing.

`DONE` is not a distinct record: it is a **full DENT struct** whose id is `DONE`.
Its body is still present and must still be read, even though it carries nothing.
`FAIL` is different again: its body is a four-byte length followed by the reason
string, exactly like the request format.

## How `list` is implemented

[`src/sync.cpp`](../src/sync.cpp) is short. Read it alongside this list:

1. Reject a path longer than 1024 bytes up front, because the daemon would reject
   it anyway (`src/sync.cpp:80`).
2. Choose the v1 or v2 form from `connection.supports_feature("ls_v2")`
   (`src/sync.cpp:87`). The match is exact, like adb's.
3. Open the `sync:` stream. This is the same `Stream` that `shell:` uses
   (`src/sync.cpp:90`).
4. Write the `LIST`/`LIS2` request as one write (`src/sync.cpp:94`).
5. Loop: read a four-byte id, then the body, then the name, and build a
   `DirEntry`. `FAIL` throws, `DONE` ends the loop (`src/sync.cpp:105`).
6. Write `QUIT` to leave sync mode, after which the daemon closes the stream
   (`src/sync.cpp:175`).

The loop reads the body **before** it checks the id, so that every path consumes
exactly the bytes the daemon sent. That is what keeps the stream aligned for the
next response; a short read here would desynchronize the whole listing.

## Two details that are not in the format

The wire format above is all a sync reference documents, but two behaviours of the
real device are not in it. Both were found with a USBPcap capture and are recorded
as blockers 17 and 18 in [`04-blockers.md`](04-blockers.md).

### The device acknowledges our write

The recipient of an ADB `WRTE` acknowledges it with an `OKAY`. The shell service
puts the command in the `OPEN` destination, so `run` never sends a `WRTE` and never
sees this. A sync request **is** a `WRTE`, so the device answers with an `OKAY` that
carries no data. `Stream::receive_more` skips `OKAY` frames and reads the next
frame instead (`src/stream.cpp`).

### The device can send a stray CLOSE

After a `run` and then a `list` in one session, the device may send a second `CLOSE`
for the **previous** stream while the next stream's `OPEN` is in flight. The `OPEN`
response read that frame and treated it as a refusal, so `list` failed with "failed to
open the stream" — but only when it followed a `run`. The fix is that frames carry
the recipient's local id in `arg1`, so `Stream` ignores any frame whose `arg1` is not
its own local id.

## Why `Stream::read` exists

The sync responses are length-prefixed and can straddle `WRTE` boundaries, so the
reader must be able to ask for an exact number of bytes and keep reading until it has
them. `Stream::read(std::span<std::byte>)` does that: it drains the buffer first,
then reads more `WRTE` payloads until the request is satisfied. `read_all`, which the
shell service uses, stops at `CLOSE` instead because a command's output length is not
known in advance.

## Try it without a device

[`examples/sync/main.cpp`](../examples/sync/main.cpp) runs a full `list` exchange
against the mock transport, so the real `list` code executes with no device attached.
It prints the entries and the `LIS2` request that `list` wrote:

```
cmake --build build --config Release --target adbcpp_sync_example
build/examples/Release/adbcpp_sync_example
```

It is also registered with CTest, so `ctest -R example_sync` runs it as part of the
normal test suite.
