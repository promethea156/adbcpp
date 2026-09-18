# Blockers and Solutions

This document records the significant problems encountered while implementing `adbcpp` and how they were resolved. It exists so that future contributors understand **why** the implementation looks the way it does, and can find better solutions than the ones recorded here. Several of these are undocumented ADB behaviours that are easy to rediscover the hard way.

Each entry has the same shape:

- **Symptom** — what was observed.
- **Cause** — what was actually going on.
- **Resolution** — what was changed.
- **Note** — alternatives or caveats worth knowing.

## Dependency Choices

### 1. USB backend: libusb, dynamically linked

- **Symptom**: The library needs USB access but must stay dependency-free and BSL-1.0.
- **Cause**: Talking to a device over USB requires a platform USB API or a third-party library.
- **Resolution**: Use **libusb**, acquired with CMake **FetchContent** from the community [`libusb/libusb-cmake`](https://github.com/libusb/libusb-cmake) build, and build it as a **shared** library (`LIBUSB_BUILD_SHARED_LIBS=ON`). libusb is LGPL-2.1-or-later, so it is deliberately linked dynamically as an **optional** backend target and never statically linked into the core library. Consumers that only need TCP or an emulator do not pull libusb in.
- **Note**: The intended long-term replacement is platform-native USB APIs (WinUSB on Windows, IOKit on macOS, `usbfs` on Linux). That removes the third-party dependency and its license obligations entirely. See `docs/01-objective.md`.

### 2. Crypto dependency: Botan replaced by mbedTLS

- **Symptom**: A crypto dependency was needed for RSA key generation and token signing.
- **Cause**: The first decision was **Botan** (BSD-2-Clause), but its CMake integration is not first-class and it is a large dependency to pull in for one RSA key pair.
- **Resolution**: Switch to **mbedTLS** (Apache-2.0), which has first-class CMake support (`MbedTLS::mbedcrypto`) and is small enough to use only for key generation, parsing, and signing.
- **Note**: mbedTLS can also provide TLS should the encrypted ADB transport turn out to be required (see the open questions in `docs/03-roadmap.md`).

## USB Transport

### 3. Header and payload are separate USB transfers

- **Symptom**: A message written as one buffer was not understood by the device.
- **Cause**: ADB over USB sends the 24-byte message header and its payload as **separate bulk transfers**, unlike TCP where they form a byte stream.
- **Resolution**: The `Session` layer writes the header and payload as two separate transport writes, and reads them as two separate reads. `UsbTransport` returns one USB transfer per `read()`, so a header and its payload can arrive as separate reads. This is harmless for byte-stream transports such as TCP.
- **Note**: If a different USB backend is written, it must preserve this framing. The `Transport` abstraction only moves bytes; `Session` assumes nothing about transfer boundaries.

### 4. Stalled USB endpoints need `clear_halt` (and sometimes a replug)

- **Symptom**: After a failed run, the next connection attempt timed out on the first bulk transfer.
- **Cause**: A bulk endpoint can be left in a halted state by a failed transfer.
- **Resolution**: `libusb_clear_halt` is called on both bulk endpoints when the transport is opened. Additionally, a failed run can leave the **device's** endpoint stalled until the device is physically replugged, so each failed attempt may need a replug.
- **Note**: The replug requirement is device-side and cannot be fixed from the host.

### 5. Device presence must be checked before opening

- **Symptom**: Running the example with no device attached produced a hard error.
- **Cause**: `libusb_open` on a missing device throws.
- **Resolution**: `UsbTransport::is_present(DeviceId)` enumerates devices and returns whether a matching one exists, without opening it. The USB example warns and exits successfully when absent, and the device integration test exits with code `77` so CTest reports it as **skipped** rather than failed.
- **Note**: If the device is present but its ADB interface is claimed by another process (for example a running adb server), the test still fails rather than skips, which is arguably correct.

## Handshake and Authentication

### 6. The CNXN banner must advertise host features

- **Symptom**: The device did not accept `shell_v2` and the handshake did not progress.
- **Cause**: AOSP's host sends `host::features=<list>` and adbd **resets its feature set from that banner**. Without a feature list the device treats the host as supporting nothing.
- **Resolution**: Send the standard host feature set in the CNXN banner. See `kSystemIdentity` in `include/adbcpp/connection.hpp`.
- **Note**: The exact feature list matters for `shell_v2` and for delayed acknowledgements. The current list is hand-maintained; deriving it from AOSP's `supported_features()` would be more robust. A USBPcap capture of adb 37.0.1 showed that the adb host advertises `...sendrecv_v2_dry_run_send,devicetracker_proto_format,devraw,app_info,server_status,track_mdns` with **no** `openscreen_mdns` (that is an adbd feature, not a host one), so it was removed from `kSystemIdentity` to match.

### 7. The OPEN/AUTH payloads are null-terminated

- **Symptom**: The device did not parse the service string or public key reliably.
- **Cause**: AOSP null-terminates the stream destination (`OPEN`) and the RSA public key (`AUTH` type 3) payloads and includes the null in `data_length`.
- **Resolution**: Append a trailing `\0` to the `OPEN` and `AUTH` public-key payloads.
- **Note**: The CNXN banner was initially null-terminated too. A USBPcap capture showed that adb's host CNXN banner is exactly 286 bytes with **no** trailing null (it passes the string length), so the CNXN null was removed in favour of the `OPEN`/`AUTH` payloads, which do need one.

### 8. The ADB private key is PKCS#8 PEM

- **Symptom**: Loading `~/.android/adbkey` failed or produced a key that the device did not recognise.
- **Cause**: adb's `adbkey` is a **PKCS#8 PEM** private key, not the older raw `RSAPrivateKey` structure. Parsing it by hand is error-prone.
- **Resolution**: Use mbedTLS's PK layer (`mbedtls_pk_parse_keyfile` / `mbedtls_pk_write_key_pem`) for parsing, storing, and signing. This also means the same key adb already authorized on the device is reused.
- **Note**: The key is stored as PKCS#8 PEM so adb and `adbcpp` can share `~/.android/adbkey`.

### 9. The ADB public key is a custom little-endian blob

- **Symptom**: The device rejected the connection even though the key was correct.
- **Cause**: ADB's public key format is not a standard DER `SubjectPublicKeyInfo`. It is a custom `RSAPublicKey` structure: `modulus_size_words` (64), `n0inv` (`-1/n[0] mod 2^32`), the modulus as **little-endian** 256 bytes, `rr` (`R^2 mod n`, little-endian), and the exponent (65537) as a little-endian `uint32_t`. The whole 524-byte blob is base64-encoded and followed by ` user@host`.
- **Resolution**: Implement the encoding in `Key::public_key()` and verify it against AOSP's `libcrypto_utils/android_pubkey.cpp`. Unit tests assert the `n0inv` identity (`n0 * n0inv == 0xFFFFFFFF`) and that `rr == R^2 mod n`.
- **Note**: AOSP's `android_pubkey_decode` explicitly **ignores** `n0inv` and `rr` (it lets BoringSSL recompute them), so a standard RSA verification is enough. They are still encoded for compatibility with older adbd implementations that use them.

### 10. AUTH type 2 signs the token with SHA-1 PKCS#1 v1.5

- **Symptom**: The device did not accept the signed AUTH response.
- **Cause**: adbd verifies with `RSA_verify(NID_sha1, token, token_size, sig, sig.size(), key)` and adb signs with `RSA_sign(NID_sha1, token, token_size, ...)`. `RSA_sign` treats the token itself as the SHA-1 **digest** and prepends the SHA-1 `DigestInfo`; it does **not** re-hash it. The signature is therefore a standard PKCS#1 v1.5 SHA-1 signature whose digest is the 20-byte token, 256 bytes long.
- **Resolution**: Sign with mbedTLS's `mbedtls_pk_sign` using `MBEDTLS_MD_SHA1` **over the token directly** (the token is the digest). Unit tests verify the signature against the modulus/exponent decoded from the public key blob.
- **Note**: The device sends a fresh random token per connection, so signatures cannot be compared directly between two runs; compare against the same token. See entry 16 for how this was finally confirmed with a capture.

### 11. Fall back to the public key when the signature is rejected

- **Symptom**: The device sent `AUTH` (token) a second time instead of `CNXN`, and the handshake threw.
- **Cause**: If the device does not recognise the signature, adbd sends `AUTH` again. adb's host then replies with the **public key** (`AUTH` type 3) to trigger the on-device approval prompt.
- **Resolution**: When a second `AUTH` arrives after sending the signature, send the public key (with a trailing null), then wait for the device's `CNXN`. `Connection::requested_authorization()` reports whether this happened.
- **Note**: This is exactly adb's behaviour and is required for the first connection to a device, or after the device's authorizations have been revoked.

## Stream and Shell

### 12. OPEN `arg1` (send buffer) depends on delayed acknowledgements

- **Symptom**: The device closed the stream immediately after `OPEN`.
- **Cause**: On transports that support delayed acknowledgements, `OPEN.arg1` is the initial flow-control window (`INITIAL_DELAYED_ACK_BYTES`). Sending `0` makes adbd close the stream.
- **Resolution**: Parse the device's `delayed_ack` feature from its banner and advertise a non-zero window when negotiated. This is configurable via `Connection`'s `advertise_delayed_ack`.
- **Note**: Matching adb's host banner (which includes `delayed_ack`) made the negotiated window non-zero and **broke** `OPEN` on the test device, so the advertisement is currently off. The `OPEN` window value may need to match the negotiated maximum payload rather than a fixed `256 KiB`.

### 13. The OPEN payload was never written (the shell blocker)

- **Symptom**: The device never answered `OPEN`, so `shell,v2,raw:<command>` never ran. The handshake and the `AUTH` exchange completed.
- **Cause**: `Stream`'s constructor passed the service payload to `make_message` but **not** to `Connection::send`. The `OPEN` header therefore advertised `data_length=24`, but the 24 payload bytes were never written. The device waited for bytes that never arrived. Comparing against a real adb connection had pointed at the `OPEN` local id and send buffer, which were red herrings; instrumenting the session showed the header advertised a payload the transport never sent.
- **Resolution**: Pass the payload to `Connection::send` as well (`src/stream.cpp`). This was the real blocker for Slice 1.
- **Note**: This class of bug (a header that disagrees with what is actually written) is easy to introduce because `Message` and `Session::send` take the payload separately. A defensive check that `header.data_length` matches the payload span would catch it.

### 14. Use the `shell_v2` service and parse its packets

- **Symptom**: Command output was empty or mangled.
- **Cause**: The device advertises `shell_v2`, and adb opens commands as `shell,v2,raw:<command>`. The `shell:` service does not provide separate stdout/stderr or an exit code.
- **Resolution**: Open `shell,v2,raw:<command>` and reassemble the shell_v2 packets: stdout (id 1), stderr (id 2), and exit (id 3). `CommandResult` carries the combined output and the exit code.
- **Note**: stdout and stderr are currently merged. If they need to be separate, `CommandResult` can be extended.

### 19. The shell_v2 exit code is in the packet's data, not its length

- **Symptom**: Every command reported exit code `1`, including `echo hello` and `true`, which exit with `0`.
- **Cause**: The exit packet was parsed as `id`, a four-byte length, and then data, with the length taken as the status. That is wrong: adbd writes the exit packet as `output_->data()[0] = exit_code; output_->Write(ShellProtocol::kIdExit, 1);` in `daemon/shell_service.cpp`, so the **length is always 1** and the single data byte is the status. A capture of `shell,v2,raw:echo hello` shows the exit packet as `03 01 00 00 00 00`: id `kIdExit`, length `1`, data `00`.
- **Resolution**: Read the exit code from the packet's data instead of its length (`src/shell.cpp`). The `DONE`-style layouts of the other packets are unchanged.
- **Note**: This went unnoticed because `run` had no unit test and the device test checked only the output, never the exit code. Both now cover it. AOSP has no document for the shell protocol (`docs/dev/` covers only the ADB protocol and `sync`), so the packet layout has to be read from `shell_protocol.h` and the daemon that writes it; our own comment had claimed the length carried the status.

## Testing and Device Interaction

### 15. The first connection after idle can time out

- **Symptom**: The first connection attempt sometimes timed out on the first bulk transfer, then succeeded on retry.
- **Cause**: Observed intermittently; the exact cause is not confirmed. It is not related to device presence (the device is enumerated) and predates the presence check.
- **Resolution**: The transfer timeout was raised to 120 seconds, which also gives the user time to approve the on-device prompt. The device integration test can be re-run.
- **Note**: This is worth revisiting with a raw USB capture. It may be a device or host controller quirk.

### 16. The device prompts for authorization on every run

- **Symptom**: The device shows the USB debugging authorization prompt on every run, even when "Always allow from this computer" is checked, and the device's authorized-computers list does not contain our key.
- **Cause**: `Key::sign` hashed the token with SHA-1 and then signed that hash as the digest, i.e. it **double-hashed** the token. adb signs the token **directly as the digest** (`RSA_sign(NID_sha1, token, ...)`, see entry 10). adbd therefore rejected every signature and sent a fresh `AUTH` token, which the host answered with the public key and which triggered the prompt. This was invisible at the protocol layer because both sides sent well-formed `AUTH` type 2 messages; only the signature bytes differed.
- **Resolution**: Remove the SHA-1 pre-hash in `Key::sign` and pass the token straight to `mbedtls_pk_sign` with `MBEDTLS_MD_SHA1` (`src/crypto/adb_key.cpp`). This was confirmed with a USBPcap capture: adb's signature verifies as `VerifyHash(token)` (and fails as `VerifyHash(SHA1(token))`), while ours did the opposite; after the fix ours also verifies as `VerifyHash(token)` and the device accepts it without a prompt.
- **Note**: The earlier "byte-for-byte identical to an independent RSA implementation" check (entry 10) compared against `SignHash(SHA1(token))`, which matched the buggy double-hash and gave false confidence. The correct comparison is `SignHash(token)`. A `key fingerprint` and a `requested_authorization` diagnostic were added to the USB example to help; the MD5 fingerprint shown on the device's authorized-computers list is computed from the decoded public key blob, while adb's log fingerprint is a SHA-256 of the DER `SubjectPublicKeyInfo`, so the two formats are not interchangeable.

## Sync and File Listing

### 17. The device acknowledges a WRITE with OKAY

- **Symptom**: `list` failed with "unexpected message on the stream" after the `LIST` request.
- **Cause**: The recipient of a `WRTE` acknowledges it with `OKAY`. The shell service puts the command in the `OPEN` destination, so `run` never sends a `WRTE` and never sees this. A sync request **is** sent as a `WRTE`, so the device answered with an `OKAY` that carried no data, and `receive_more` treated it as unexpected.
- **Resolution**: `Stream::receive_more` skips `OKAY` frames and reads the next frame instead (`src/stream.cpp`). This is the same behaviour as adb, whose socket layer consumes the acknowledgement.
- **Note**: The `OKAY` also appears with delayed acknowledgements, where its payload is the acknowledged byte count. Skipping it is correct in both cases.

### 18. The device sends a second CLOSE for a previous stream

- **Symptom**: After `run` then `list` in one session, the `sync:` `OPEN` was refused with `CLSE` and `list` failed with "failed to open the stream". Running `list` on its own worked.
- **Cause**: The device may send a second `CLOSE` for the previous stream, with the previous stream's ids in `arg0`/`arg1`, while the next stream's `OPEN` is in flight. The `OPEN` response read that frame and treated it as a refusal. A USBPcap capture showed the stray `CLOSE` arriving between the `sync:` `OPEN` and its `OKAY`.
- **Resolution**: Frames carry the recipient's local id in `arg1`, so `Stream` skips any frame whose `arg1` is not its own local id, both while opening and while reading (`src/stream.cpp`).
- **Note**: The same applies to `WRTE` and `OKAY`: a frame for another stream must never be mistaken for this stream's data. The protocol multiplexes streams over the one connection, so a stream must always check the ids.

## Cross-Platform Build

### 20. libusb's udev backend needs `libudev.h` on Linux

- **Symptom**: The Linux CI build failed while compiling libusb: `linux_udev.c:28:10: fatal error: libudev.h: No such file or directory`. Windows was unaffected.
- **Cause**: libusb's CMake option `LIBUSB_ENABLE_UDEV` defaults to `ON` on Linux, which compiles `os/linux_udev.c` and links `libudev`. The `libudev` headers are a system package and are not present on a default Ubuntu runner, or on a default Linux toolchain.
- **Resolution**: Set `LIBUSB_ENABLE_UDEV OFF` before fetching libusb (`src/usb/CMakeLists.txt`). libusb then uses its netlink backend (`os/linux_netlink.c`), which enumerates devices through sysfs and watches hotplug through a netlink socket, so no system package is needed at build time.
- **Note**: This matches the project's "self-contained, no external dependency" goal (blocker 1). It only affects how libusb builds; device access still depends on the usual runtime udev rules, which is a separate concern.

### 21. `$<TARGET_RUNTIME_DLLS>` is empty on non-DLL platforms

- **Symptom**: The macOS build failed after linking the USB example, with a `cmake -E copy_if_different` error.
- **Cause**: The POST_BUILD step copies `$<TARGET_RUNTIME_DLLS:...>` next to the executable so the libusb DLL is found at runtime. CMake documents that this generator expression "always evaluates to an empty string" on non-DLL platforms, and the empty argument is then dropped, leaving `cmake -E copy_if_different <dir>` with a single argument, which is an error.
- **Resolution**: Guard the copy with `if(WIN32)` in `examples/CMakeLists.txt` and `tests/CMakeLists.txt`. On Linux and macOS the shared library is found through the build-tree RPATH, so no copy is needed.
- **Note**: The same trap applies to any POST_BUILD step built from `TARGET_RUNTIME_DLLS`; on a non-DLL platform the command has to be skipped, not just have its arguments ignored.

### 22. mbedTLS compiles with `-Werror` and a newer Clang warns

- **Symptom**: The macOS build failed inside mbedTLS: `ssl_tls13_keys.c:44:5: error: initializer-string for character array is too long ... [-Werror,-Wunterminated-string-initialization]`, twenty times over, for the `MBEDTLS_SSL_TLS1_3_LABEL_LIST` entries.
- **Cause**: mbedTLS's `MBEDTLS_FATAL_WARNINGS` option defaults to `ON` and adds `-Werror`. A newer AppleClang enables `-Wunterminated-string-initialization` by default, and mbedTLS's TLS 1.3 labels are deliberately not NUL-terminated (the array is sized to `sizeof(label) - 1`), so the warning fires and becomes an error.
- **Resolution**: Set `MBEDTLS_FATAL_WARNINGS OFF` before fetching mbedTLS (`src/crypto/CMakeLists.txt`). The warning is a false positive in mbedTLS, and a dependency's warnings should not be able to fail this project's build.
- **Note**: This is a compiler-version effect, not a code bug: the same mbedTLS version builds on Windows and on the older Clang that ships with the older macOS runner images. Disabling fatal warnings for third-party code is the conventional fix.

## File Transfer

### 23. A pulled file must be closed before it can be removed on Windows

- **Symptom**: The device-free sync example crashed with `STATUS_STACK_BUFFER_OVERRUN` after a successful `pull`, with no output, while the same code passed as a unit test.
- **Cause**: The example read the pulled file back and then called `std::filesystem::remove` while the `std::ifstream` was still open. Windows refuses to delete a file that another handle has open, so `remove` threw `std::filesystem::filesystem_error`, and the unhandled exception terminated the process. The unit test helper returned the string and closed the stream on return, so it was unaffected.
- **Resolution**: Read the file inside its own scope so the stream is destroyed before `remove` runs.
- **Note**: `std::filesystem::remove` has both a throwing and an `error_code` overload; this is a case where the throwing one is the less forgiving default. The same applies to renaming or replacing a file that is still open.

### 24. A large transfer can exceed the per-transfer timeout

- **Symptom**: Pulling a 128 MiB or larger file failed with `libusb_bulk_transfer: Operation timed out` after about 130 seconds, and the local file was left truncated at 127 MiB. Everything up to 64 MiB round-tripped reliably.
- **Cause**: The USB transport bounded each bulk transfer at a fixed 120 seconds. The timeout measures **silence, not transfer duration**: libusb gives up when the peer sends nothing at all for that long, so a 64 KiB chunk that takes milliseconds on the wire still fails if the device stops delivering it. On one run the device stalled a single transfer for longer than the timeout. Throughput is otherwise at parity with `adb` (about 8 MB/s each way on the test device), so this is a slow-device tolerance problem rather than a protocol defect.
- **Resolution**: The per-transfer timeout is now short and a transfer is retried while it times out having moved nothing, bounded by a longer budget: `kDefaultTransferTimeoutMs` (5 s) per transfer and `kDefaultTransferBudgetMs` (120 s) in total, both configurable through the constructor and setters, and exposed as `--timeout <ms>` and `--budget <ms>` on the USB example. libusb is careful not to lose data it did transfer, so `transferred` decides the outcome: a timeout that moved nothing is retried, a partial read is handed back as a short read, and a partial write is a desync and is reported immediately. `push` also writes the sync `DATA` header and its chunk in one message instead of two, halving the messages and acknowledgements per chunk.
- **Note**: The retry design deliberately keeps the per-transfer timeout short, so a device that has gone quiet is noticed quickly, while the budget is what still allows the human-paced approval of the on-device debugging prompt, which is the one legitimate long silence. Verified with 64 MiB, 128 MiB, 256 MiB, and 1 GiB SHA-256 round-trips, a pull at a 1 ms per-transfer timeout (which succeeds through short reads), and a push at the same timeout (which fails immediately with a desync, as it must). The timings and rates are recorded in [`06-sync-protocol.md`](06-sync-protocol.md#measured-transfer-performance).

## Application Management

### 25. `pm install` throws instead of printing `Failure [...]`

- **Symptom**: `install` of a file that is not an APK reported `success == false`, as it should, but `failure_reason()` came back empty. The device's output was a Java stack trace, not a `Failure [REASON]` line.
- **Cause**: `pm` prints `Failure [REASON]` only for a package manager error. When the APK cannot even be parsed, `PackageManagerShellCommand.setParamsSize` throws `java.lang.IllegalArgumentException: Error: Failed to parse APK file: ...`, and the shell prints `Exception occurred while executing 'install':` followed by the stack trace. The exit code is still nonzero, so `success` is right, but there is no bracket to extract. `pm uninstall` of a package that is not installed does print `Failure [DELETE_FAILED_INTERNAL_ERROR]`, so the two commands report a rejection differently.
- **Resolution**: `PackageResult::failure_reason()` returns the text inside `Failure [...]` and an empty string otherwise, while `output` always holds the device's answer verbatim. The device integration test therefore checks a non-empty output rather than a non-empty reason for this case.
- **Note**: The exit code is the authority on whether the command worked and the output is the explanation, because the two commands do not agree on the form of the explanation. The APK is pushed into `/data/local/tmp` first, where the daemon widens `0644` to `0666` (blocker 23), and `pm install` accepts it from there; installing from the file is what `adb install` does too.

## Crypto

### 26. `mbedtls_pk_write_key_pem` returns zero on success

- **Symptom**: The key generation path had never run, because `~/.android/adbkey` already existed from adb.
- **Cause**: `mbedtls_pk_write_key_pem` returns **zero on success** and writes a NUL-terminated string, but the code treated the return value as a length: it returned an error when the value was zero, so it failed on success, and would have written a negative length on failure.
- **Resolution**: Treat non-zero as the failure, and take the length from the string itself with `std::char_traits<char>::length`. Verified by pointing the home directory at a scratch directory, generating a key, and reloading it to confirm the same public key comes back.
- **Note**: Nothing exercised this, because the tests call `Key::generate` (which does not write files) and the device already had adb's key, so `load_or_generate` always took the early return. A code path that only runs on a fresh machine is worth a test.

## Protocol Validation

### 27. The "CRC" is a plain byte sum, and a strict device ignores a real CRC

- **Symptom**: A second test device (a REDMAGIC 9S Pro) ignored every CNXN, so the handshake never started, while the first device accepted it. The CNXN's `data_crc32` field was correct: a CRC-32 of the banner.
- **Cause**: `docs/dev/protocol.md` names the field `data_crc32`, but AOSP's `calculate_apacket_checksum` is a plain **sum of the payload bytes** and its field is `data_check`. adbd verifies that sum on the CNXN and AUTH messages, so a real CRC-32 fails it and the device does not answer. The first device was lenient and accepted either.
- **Resolution**: `Message::compute_checksum` adds the payload bytes, matching `calculate_apacket_checksum`, and the field is renamed `data_check` to match AOSP. A capture of adb confirms the value: its 286-byte banner sums to `0x701d`, exactly the CNXN `data_check` adb sends.
- **Note**: The checksum only matters during the handshake: AOSP's `send_packet` computes it only while the protocol is below `A_VERSION_SKIP_CHECKSUM`, so the CNXN and AUTH carry it and every later message carries zero. `Session::receive` therefore still verifies it only when non-zero. The unit test vector is now the byte sum of "123456789" (`0x1dd`), not the CRC-32 check value.

## Parallel Devices

### 28. WinUSB allows one handle per device, so one device cannot be opened twice

- **Symptom**: Two `UsbTransport`s to the same device, on two threads or in two processes that overlap, fail the second `libusb_open` with `LIBUSB_ERROR_ACCESS` ("Access denied"). Two different devices are fine, and the same device works again once the first handle is closed.
- **Cause**: The device's ADB interface is bound to the Microsoft WinUSB driver with a single `DeviceInterfaceGUID` registry value (`{F72FE0D4-...}` on the test device). WinUSB admits one handle per device interface unless the INF registers `DeviceInterfaceGUIDs` (plural), so a second `CreateFile` is denied. It is not the `libusb` context (each transport has its own) or a race (the denial is deterministic and happens even when the opens are sequential).
- **Resolution**: The supported model is one thread per device, and two devices are needed to exercise it. `examples/multi` opens two and, when both selectors name the same single device, reports that a second device is needed.
- **Note**: This is why `adb` and this library cannot hold the device at the same time either (the `AGENTS.md` rule to stop `adb` first), and it is a driver property rather than a host one: a device whose INF registers multiple interface GUIDs would allow both. Two emulators over TCP are unaffected, since each is its own endpoint.

## USB 3 Devices

### 29. A device resets its USB 3 link around the open and stalls the first write

- **Symptom**: On one device (a REDMAGIC 9S Pro, a USB 3 device) about half of the runs failed the first `libusb_bulk_transfer` with `Pipe error` or `Input/Output Error`, and the device then vanished from enumeration for a moment. The other device (a USB 2 phone) never failed.
- **Cause**: The device resets its USB 3 link just after `libusb_open`/`libusb_claim_interface` and before the host's first write. The CNXN is written while the link is down and is discarded, so the device never answers and the read finds a stalled endpoint. A USBPcap capture shows USB 3 link-management frames on the device's address immediately before the host's CNXN, and no reply.
- **Resolution**: Re-opening the transport and re-running `Connection::connect` recovers, so `examples/usb` and `examples/multi` retry the whole open and handshake a few times with a short delay. Clearing the endpoint's halt inside the one transport was not enough, because the CNXN itself had already been lost.
- **Note**: `adb` sends the CNXN twice, which is the same retry at a lower level. The retry belongs at the open rather than in `Session`, because a device that re-enumerates invalidates the handle, and because `Session::receive` closes the transport on a framing error.
