# References

The following external resources serve as references for understanding the ADB protocol and its implementation. They are essential for building a client that communicates directly with devices without relying on Google's ADB server or CLI.

## ADB Protocol Documentation

### cstyan/adbDocumentation

- **URL**: https://github.com/cstyan/adbDocumentation
- **What it is**: A community reverse-engineering effort that documents the ADB protocol in far more detail than the AOSP source, with a focus on USB transport.
- **Why it matters**:
  - Explains the actual connection handshake (CNXN, AUTH) and the packet format (`command`, `arg1`, `arg2`, `data_length`, `data_crc32`, `magic`).
  - Documents the USB quirk where the ADB packet and its payload are sent as separate USB transfers.
  - Describes the `sync` subcommands (SEND, RECV, LIST, STAT, QUIT) and the push/pull/list flows step by step.
  - Notes the `LIS2`/`DNT2` variant used for files larger than ~2.14 GB, and the `libmincrypt`-based RSA token signing used during authentication.

## Articles

### Diving into ADB protocol internals (1/2)

- **URL**: https://www.synacktiv.com/en/node/1042
- **What it is**: A Synacktiv blog post introducing ADB's architecture (client, server, `adbd` daemon) and the two distinct protocols used: client-to-server and server-to-device.
- **Why it matters**:
  - Explains the `host:` and `local:` service model and the length-prefixed (LTV) request format.
  - Covers `sync` operations, authentication, and security considerations.
  - Motivates exactly our goal: eliminating the system package and external binary dependencies by talking to the protocol directly.

### Diving into ADB protocol internals (2/2)

- **URL**: https://www.synacktiv.com/publications/diving-into-adb-protocol-internals-22
- **What it is**: The follow-up post covering how to connect directly to an end device (USB/TCP) without the ADB server, including client authentication handled by `adbd`.
- **Why it matters**: This is the serverless, direct-device path our library targets.

## Reference Implementations

### Synacktiv/adb_client

- **URL**: https://github.com/Synacktiv/adb_client
- **What it is**: A pure-Rust ADB client library implementing both the server and end-device protocols, plus a CLI (`adb_cli`) and a Python wrapper (`pyadb_client`).
- **Why it matters**:
  - Demonstrates a real, working design for connecting **directly to devices** over USB and TCP/IP without the ADB server.
  - Shows a clean high-level API abstraction (`ADBServer`, `ADBServerDevice`) that can inspire our C++ API.
  - Documents hidden features (e.g. `framebuffer`) and supports mDNS device discovery.
  - Serves as a close analogue to what we want to build, in a different language.

## Related AOSP Sources

For completeness, the upstream protocol documents referenced by the resources above. The old `protocol.txt` / `OVERVIEW.TXT` / `SYNC.TXT` / `SERVICES.TXT` files were moved into `docs/dev/` and renamed:

- Protocol: https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/docs/dev/protocol.md
- Overview: https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/docs/dev/overview.md
- Sync: https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/docs/dev/sync.md
- Services: https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/docs/dev/services.md
- Delayed acknowledgements: https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/docs/dev/delayed_ack.md
- USB serial selection (`LibUsbDevice::RetrieveSerial`): https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/client/usb_libusb_device.cpp
