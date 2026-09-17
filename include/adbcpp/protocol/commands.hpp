#pragma once

#include <cstdint>

// The ADB wire protocol is, at its core, a small set of messages exchanged over
// a byte channel (USB bulk transfers, or TCP). Each message is a 24-byte header
// followed by an optional payload; the header carries a 32-bit "command" constant
// and two generic arguments whose meaning depends on the command.
//
// The only canonical description of the protocol is AOSP's `docs/dev/protocol.md`:
//
//   https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/docs/dev/protocol.md
//
// It documents each command as COMMAND(arg0, arg1, "payload"), for example
// OPEN(local-id, 0, "destination"). The constants below are the four-character
// command identifiers from its "message command constants" section.

namespace adbcpp::protocol {

/// Packs four ASCII characters into an ADB command identifier.
///
/// Each command constant is literally the little-endian encoding of four ASCII
/// characters, so `CNXN` is 'C' | 'N' << 8 | 'X' << 16 | 'N' << 24. Building
/// them from characters keeps this list readable against AOSP's `#define A_CNXN
/// 0x4e584e43` style. AOSP defines them in `adb.h`:
///
///   https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/adb.h
constexpr std::uint32_t make_command(char a, char b, char c, char d) noexcept {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24);
}

/// `CNXN`: CONNECT(version, maxdata, "system-identity-string").
///
/// The first message on a connection. Both sides send one; the payload is the
/// sender's identity and, as a `host::` string, the features it supports. See
/// `kSystemIdentity` in `connection.hpp`.
inline constexpr std::uint32_t kCnxn = make_command('C', 'N', 'X', 'N');

/// `AUTH`: AUTH(type, 0, "data").
///
/// The device asks the host to authenticate; the host answers with a signature or
/// with its public key. See the `kAuth*` constants below.
inline constexpr std::uint32_t kAuth = make_command('A', 'U', 'T', 'H');

/// `OPEN`: OPEN(local-id, 0, "destination").
///
/// Opens a logical stream to a service on the device, named by the payload (for
/// example `shell,v2,raw:echo hello`). See `stream.cpp`.
inline constexpr std::uint32_t kOpen = make_command('O', 'P', 'E', 'N');

/// `OKAY`: READY(local-id, remote-id, "").
///
/// Acknowledges an `OPEN` or a `WRTE`. With delayed acknowledgements its payload
/// carries how many bytes were received, which is what releases the peer's window.
inline constexpr std::uint32_t kOkay = make_command('O', 'K', 'A', 'Y');

/// `CLSE`: CLOSE(local-id, remote-id, "").
///
/// Closes a stream, or rejects an `OPEN`. The protocol is strict about who may
/// send it: the receiver of a `WRTE` that violates the flow-control rule MUST
/// `CLSE` the stream.
inline constexpr std::uint32_t kClse = make_command('C', 'L', 'S', 'E');

/// `WRTE`: WRITE(local-id, remote-id, "data").
///
/// Sends data on a stream. A `WRTE` may not be sent until the stream is ready,
/// and no further `WRTE` may be sent until the previous one is acknowledged with
/// an `OKAY`.
inline constexpr std::uint32_t kWrte = make_command('W', 'R', 'T', 'E');

/// `SYNC`: SYNC(online, sequence, "").
///
/// Marked obsolete in `docs/dev/protocol.md`: it was an internal mechanism to
/// discard stale messages when the link broke. It is defined only for completeness.
inline constexpr std::uint32_t kSync = make_command('S', 'Y', 'N', 'C');

/// ADB protocol version advertised in the CNXN message.
///
/// `docs/dev/protocol.md` documents `0x01000000` (it is stale on this point),
/// while a USBPcap capture of adb 37.0.1 shows the host and device both sending
/// `0x01000001`. `A_VERSION` in AOSP's `adb.h` is the source of truth.
inline constexpr std::uint32_t kVersion = 0x01000001u;

/// Maximum payload size advertised in the CNXN message. Matches adb's `MAX_PAYLOAD`.
///
/// The value must be large enough to hold a 256-byte AUTH signature plus its
/// header. `docs/dev/protocol.md` still says `256 * 1024` and notes that older
/// adb hard-coded `4096`; a USBPcap capture of adb 37.0.1 shows `0x100000`
/// (1 MiB), which both the host and the device advertise.
inline constexpr std::uint32_t kMaxData = 1024 * 1024;

/// AUTH payload type values (the `arg0` field of an AUTH message).
///
/// From `docs/dev/protocol.md`'s AUTH section: the device sends type 1 (a random
/// token), the host answers type 2 (the token signed with its private key) or, if it
/// cannot sign, type 3 (its RSA public key) to request on-device approval.
inline constexpr std::uint32_t kAuthToken = 1;
inline constexpr std::uint32_t kAuthSignature = 2;
inline constexpr std::uint32_t kAuthPublicKey = 3;

/**
 * Initial flow-control window advertised in an OPEN message.
 *
 * Transports that support delayed acknowledgements require a non-zero send buffer
 * size in `arg1`; otherwise the peer closes the stream immediately. This is the
 * "available send bytes" (ASB) concept from AOSP's `docs/dev/delayed_ack.md`:
 *
 *   https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/docs/dev/delayed_ack.md
 *
 * The document shows adb opening with `arg1=1MiB`, and notes that a peer which
 * does not support the feature `A_CLSE`s the connection on a non-zero `arg1`.
 */
inline constexpr std::uint32_t kInitialDelayedAckBytes = 256 * 1024;

} // namespace adbcpp::protocol
