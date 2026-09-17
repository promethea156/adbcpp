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
| `kRecvV1`  | `RECV` | retrieve a file                |
| `kQuit`    | `QUIT` | leave sync mode                |

Responses:

| Constant   | ASCII  | Meaning                        |
| ---------- | ------ | ------------------------------ |
| `kDentV1`  | `DENT` | a directory entry              |
| `kDentV2`  | `DNT2` | a directory entry, v2          |
| `kData`    | `DATA` | a chunk of a file              |
| `kDone`    | `DONE` | the listing or transfer is over |
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

## Pulling a file

`RECV` retrieves a file. Its request is the same `id` + `path_length` + `path` form
as `LIST`, and the response is a stream of chunks:

```
 0               4               8
 +---------------+---------------+-----------------------+
 |      DATA     |     size      |    size bytes ...     |
 +---------------+---------------+-----------------------+
```

Each `DATA` chunk carries at most 64 KiB, which is `SYNC_DATA_MAX` in
`file_sync_protocol.h`. The device repeats `DATA` until the whole file has been
sent, then sends `DONE`:

```
 0               4
 +---------------+---------------+
 |      DONE     |   (ignored)   |
 +---------------+---------------+
```

`DONE` here is a `sync_data` record, the same shape as `DATA`, **not** the DENT
struct that ends a listing. Its size field is ignored, but it still has to be read. A
request the device cannot satisfy, for example a missing file, arrives as `FAIL`
instead, exactly as it does for `LIST`.

### Worked example: pulling `/sdcard/file.txt`

The request is the same shape as `LIST`, with the `RECV` id:

```
52 45 43 56   10 00 00 00   2f 73 64 63 61 72 64 2f 66 69 6c 65 2e 74 78 74
R  E  C  V    16 (LE)         /  s  d  c  a  r  d  /  f  i  l  e  .  t  x  t
```

The device then answers with the file's chunks and `DONE`:

```
44 41 54 41   06 00 00 00   68 65 6c 6c 6f 20      DATA "hello "
44 41 54 41   06 00 00 00   77 6f 72 6c 64 0a      DATA "world\n"
44 4f 4e 45   00 00 00 00                           DONE
```

### RECV v2

`RCV2` is the v2 form. It takes the same `id` + `path_length` + `path` request,
followed by an extra 8-byte setup packet, `sync_recv_v2 { id, flags }`, that selects a
compression codec (`kSyncFlagBrotli`, `kSyncFlagLz4`, or `kSyncFlagZstd`). With
`flags = 0` the transfer is byte-for-byte the v1 form. `pull` uses v1 because it does
not implement decompression; the `sendrecv_v2` feature that the device advertises only
matters when a codec is requested.

## Pushing a file

`SEND` stores a file. Its request is the `id` + `path_length` + `path` form again,
but the path is a **spec**: the destination path, a comma, and the file mode as a
decimal number that includes the file type bits, for example
`/sdcard/upload.txt,33188` (`33188` is `0100644`). adb sends the local file's
`st_mode`; the daemon splits the spec on its last comma and parses the mode with
`strtoul(..., 0)`.

The contents follow as `DATA` chunks, exactly as they do for `RECV`, and a `DONE`
whose size is the file's modification time ends the transfer:

```
53 45 4e 44   18 00 00 00   /sdcard/upload.txt,33188      SEND
44 41 54 41   06 00 00 00   68 65 6c 6c 6f 20            DATA "hello "
44 41 54 41   06 00 00 00   77 6f 72 6c 64 0a            DATA "world\n"
44 4f 4e 45   <mtime>                                       DONE
```

Unlike a listing or a transfer, the device **answers** the final `DONE`, with `OKAY`
when the file was written or `FAIL` and a reason when it was not:

```
4f 4b 41 59   00 00 00 00                                    OKAY
```

The device creates the file, or overwrites it if it already exists, and copies the
user permission bits to the group and other bits, so a `0644` local file becomes
`0666` on the device. That is the daemon's behaviour, not this library's.

## STAT

`STAT` reports a path's metadata and follows symbolic links, so a destination that
is a symlink to a directory is reported as a directory. The v1 form is
`sync_stat_v1 { id, mode, size, mtime }`:

```
+---------------+---------------+---------------+---------------+
|      STAT     |     mode      |     size      |     mtime     |
+---------------+---------------+---------------+---------------+
```

The v2 form, `STA2`, is the v2 `DENT` body without the name, so it inserts an
`error` before the metadata and widens `size` and `mtime` to 64 bits. Unlike the v1
form, which reports a path that does not exist as all zeros, it reports the error
directly, so a missing path can be told from a real one.

## How `list`, `pull`, and `push` are implemented

[`src/sync.cpp`](../src/sync.cpp) is short. Read it alongside this list.

`list`:

1. Reject a path longer than 1024 bytes up front, because the daemon would reject
   it anyway (`src/sync.cpp:126`).
2. Choose the v1 or v2 form from `connection.supports_feature("ls_v2")`
   (`src/sync.cpp:133`). The match is exact, like adb's.
3. Open the `sync:` stream. This is the same `Stream` that `shell:` uses
   (`src/sync.cpp:136`).
4. Write the `LIST`/`LIS2` request as one write (`src/sync.cpp:139`).
5. Loop: read a four-byte id, then the body, then the name, and build a
   `DirEntry`. `FAIL` throws, `DONE` ends the loop (`src/sync.cpp:145`).
6. Write `QUIT` to leave sync mode, after which the daemon closes the stream
   (`src/sync.cpp:207`).

The loop reads the body **before** it checks the id, so that every path consumes
exactly the bytes the daemon sent. That is what keeps the stream aligned for the
next response; a short read here would desynchronize the whole listing.

`pull`:

1. Reject a path longer than 1024 bytes, the same check as `list`
   (`src/sync.cpp:308`).
2. Open the `sync:` stream and write the `RECV` request
   (`src/sync.cpp:312`, `src/sync.cpp:317`).
3. Open the local file before the transfer starts, so a failure leaves a partial
   file rather than a missing one (`src/sync.cpp:321`).
4. Loop: read the id and the chunk size, write each `DATA` chunk to the file as it
   arrives, and stop at `DONE` (`src/sync.cpp:327`). A chunk larger than 64 KiB is
   rejected rather than trusted (`src/sync.cpp:357`).
5. Write `QUIT` (`src/sync.cpp:373`).

Because each chunk is written as it arrives, the file is never held in memory whole,
so pulling a large file costs no more memory than pulling a small one.

`stat`:

1. Reject a path longer than 1024 bytes (`src/sync.cpp:380`).
2. Choose the v1 or v2 form from `connection.supports_feature("stat_v2")`
   (`src/sync.cpp:385`).
3. Open the `sync:` stream and write the `STAT`/`STA2` request
   (`src/sync.cpp:387`, `src/sync.cpp:388`).
4. Read the id and the body. The v2 form's leading error field, or the v1 form's
   all-zero body, means the path does not exist, which is reported rather than
   thrown (`src/sync.cpp:405`).
5. Write `QUIT` (`src/sync.cpp:428`).

`push`:

1. Reject a local path that is not a regular file (`src/sync.cpp:435`).
2. Stat the destination, so that an existing directory receives the file under the
   local file's name (`src/sync.cpp:443`).
3. Build the `"<path>,<mode>"` spec from the local file's permissions
   (`src/sync.cpp:453`).
4. Open the `sync:` stream and write the `SEND` request
   (`src/sync.cpp:455`, `src/sync.cpp:456`).
5. Read the local file in 64 KiB chunks, write each as a `DATA` chunk, and finish
   with a `DONE` that carries the local file's modification time
   (`src/sync.cpp:494`).
6. Read the device's reply, which is the only `OKAY` in the sync service
   (`src/sync.cpp:499`), and write `QUIT` (`src/sync.cpp:501`).

## Measured transfer performance

A round-trip was timed on a real device: a file of random bytes is pushed to
`/data/local/tmp`, pulled back, and the two are compared by SHA-256, so a length or an
ordering error anywhere in the transfer would be caught. The host is a Windows machine
over USB 2.0. For comparison, `adb` on the same device and file measures 8.0 MB/s
pushing and 12.5 MB/s pulling, so the two are comparable.

| size | push | push rate | pull | pull rate | verified |
| ---- | ---- | --------- | ---- | --------- | -------- |
| 64 MiB | 9.4 s | 6.8 MB/s | 5.3 s | 12.0 MB/s | SHA-256 |
| 128 MiB | 18.6 s | 6.9 MB/s | 10.5 s | 12.2 MB/s | SHA-256 |
| 256 MiB | 37.0 s | 6.9 MB/s | 21.2 s | 12.1 MB/s | SHA-256 |
| 1 GiB | 147.5 s | 6.9 MB/s | 84.5 s | 12.1 MB/s | SHA-256 |

Pushing is slower than pulling because `SEND` is acknowledged per chunk while `RECV` is
not, so every pushed chunk costs a round trip. The rates are otherwise flat across three
orders of magnitude of file size, which is what the chunking is for: a 1 GiB file is 16384
chunks of 64 KiB, each with its own transfer timeout, so a long transfer is never given a
single deadline. The 1 GiB push takes longer than the 120 s transfer budget for the same
reason, and is unaffected by it.

The two error paths were exercised by dropping the per-transfer timeout to 1 ms. A pull then
succeeds through short reads, because a partial read is usable, while a push fails
immediately with `short USB write: the stream is desynchronized`, because sending the rest of
a half-delivered message would corrupt the stream. See blocker 24 in
[`04-blockers.md`](04-blockers.md).

[`tools/bench-transfer.ps1`](../tools/bench-transfer.ps1) runs the sweep above and prints
the table, so anyone with a device can reproduce it. Stop the adb server first, because adb
holds the device's USB interface while it runs (blocker 5), and pass a larger `-Sizes` list to
go further than the default 1 GiB:

```
. ./tools/invoke-adb.ps1
Invoke-Adb -Arguments @("kill-server")
./tools/bench-transfer.ps1
./tools/bench-transfer.ps1 -Sizes 1073741824
```

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

[`examples/sync/main.cpp`](../examples/sync/main.cpp) runs a full `list`, `pull`, and
`push` exchange against the mock transport, so the real code executes with no device
attached. It prints the entries, the pulled file's contents, and the `LIS2`, `RECV`, and
`SEND` request headers that were written:

```
cmake --build build --config Release --target adbcpp_sync_example
build/examples/Release/adbcpp_sync_example
```

It is also registered with CTest, so `ctest -R example_sync` runs it as part of the
normal test suite.
