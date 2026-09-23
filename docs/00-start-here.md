# Start here

A plain-language tour of what this project is, where it is, and why. No ADB
knowledge is assumed, and every term is defined where it first appears. The other
documents are written for someone who already knows the domain, so read this one
first.

## What this project is

This is a C++ library that talks to an Android phone or tablet directly, over a
USB cable or over the network. It is meant to be embedded in another C++ program,
which then runs shell commands, lists and transfers files, installs and removes
apps, and starts and stops them.

The important part is what it does **not** need. Google's own tooling and most
other projects start a background server, or spawn a command-line tool, or depend
on a program called `adb`. This library does none of that: it opens the cable
itself and speaks the phone's protocols on its own.

## The phone is a locked building

The easiest way to hold the whole thing in your head is to picture the phone as a
locked building:

- The **USB cable** is the front door. Opening it means finding the phone and
  claiming the right channel on the cable. A network address is a second front
  door, for a phone that listens over the network.
- The **resident** is a service on the phone called `adbd` (the ADB daemon). It is
  the only thing the library ever talks to; every other capability is a room it
  opens on request.
- **The handshake** is announcing yourself at the door. The two sides exchange a
  short message named `CNXN` that says who they are and which optional features
  they support.
- **Authorization** is showing ID. The first time, the phone asks a human to tap
  *Allow USB debugging?*; after that, a saved key gets the visitor in without
  asking again.
- **Services** are the rooms. The resident hands out a channel for a room by name
  (`shell`, `sync`, and so on), and the visitor talks to that room over the cable.
- **Many conversations at once** are possible on one cable. The phone mixes them
  together, and a **stream** is one such conversation, kept apart from the others
  by a small id on every message.

## What is already built

The library can already do all of the everyday work:

- find the phone and open the cable, or reach one over the network;
- authorize with it once, and remember the authorization;
- run a shell command and read its combined output and exit code;
- list a folder, check a path, and copy a file to or from the phone, packed smaller
  on the way across when that helps;
- install and remove an app, launch it, check whether it is running, and close it;
- drive every attached phone at once, one thread per phone.

The whole [initial scope](01-objective.md#initial-scope) is done, and the library
is at **2.2.1**.

## The wall we hit

The first connection to a real phone did not work. The library opened the cable,
sent its hello, and the phone stayed silent. Nothing on our side crashed; the phone
simply refused to answer.

The ADB protocol is only partly written down. The public documents describe the
shape of each message but not every value inside it, and the phone is strict about
the ones they leave out. Three details, each easy to get subtly wrong, had to be
found by capturing what the real `adb` tool sends and comparing it byte for byte:

- The field named **`data_crc32`** is not a CRC at all. It is a plain sum of the
  message's bytes, and a phone that checks it rejects a real CRC.
- The **signature** that proves the visitor's identity is made over a challenge the
  phone sends, but over that challenge *itself*, not over a hash of it. Hashing it
  first looks correct and is silently rejected.
- The payloads that name a service and send a public key end in a **null byte**,
  which counts toward their length.

None of these are visible in a debugger, because both sides send well-formed
messages either way; only the bytes differ. This is the genuine wall of the project:
the library has to match a real implementation exactly, and the specification alone
is not enough.

## What was built on the other side

With the wall down, the rest of the initial scope turned out to be **composition**,
not new protocol work. A file transfer is the `sync` room, an app install is a file
push followed by a shell command, and app control is three more shell commands
(`monkey`, `am force-stop`, and `pidof`). Nothing at the protocol layer had to
change.

The last pieces were reach and identity:

1. **The network front door.** A second transport speaks to a phone that listens over
   the network, so an emulator or a `tcpip` phone works with every feature
   unchanged.
2. **Telling two phones apart.** A phone reports a unique serial number on the cable,
   and the library can select one by it, so two phones of the same model are no
   longer both the first match.

## Packing the file smaller

The cable is the narrow part of a file transfer, and every byte has to cross it. So
the library can **compress** a file before it sends it: it packs the bytes into fewer
bytes here, sends the smaller pile, and the phone unpacks it back into the file. A file
coming the other way is packed by the phone and unpacked here.

Packing only helps when the file has repetition to squeeze out. A text file does, and
moves roughly twenty times faster with packing than without. A photo or an app package
is already packed, so it shrinks little and moves no faster; the library still makes the
attempt, because it cannot know in advance whether a file will shrink, but the attempt buys
nothing there.

There are three packing methods, and they trade speed against how small the pile gets:

- **LZ4** is the fastest and the loosest. It barely looks ahead, so it is quick but
  leaves the pile large.
- **Brotli** is the tightest and the slowest. It looks far ahead, so it makes the
  smallest pile but costs the most time to pack.
- **Zstd** sits between them: nearly as quick as LZ4 and nearly as small as Brotli.
  It is the one `adb` itself prefers.

The library's default is to **use the best method both sides know, preferring Zstd,
then LZ4, then Brotli**, and to send the file unpacked when neither side knows one.
Zstd comes first for the same reason `adb` puts it first: it is the best trade of speed
against size, so someone who does not care gets the most benefit for the least cost. A
caller who wants a specific method can ask for it, and a caller who wants no packing at
all can ask for that too.

## Where it is now

The library is complete for its current scope: every feature works end to end, and
the sample program walks them all once. It connects, runs a shell command, lists and
copies files, installs an app, starts it, checks it is running, and removes it. The
same tour can run on every attached phone at once, one thread per phone.

What is left is in [Future Improvements](03-roadmap.md#future-improvements):
platform-native USB in place of the third-party dependency, and verifying the build on
Linux and macOS.

## The words this project uses

| Word | What it means here |
| --- | --- |
| `adb`, `adb.exe` | Google's Android Debug Bridge tool and its background server. This project replaces them, so it does not need them. |
| `adbd` | The daemon on the phone that answers the library. Every service is reached through it. |
| `CNXN` | The hello both sides exchange when the cable opens, naming who they are and which features they support. |
| `AUTH` | Showing ID. The phone sends a challenge and the library signs it; the first time, a human approves on screen. |
| `adbkey` | The saved key pair that proves this computer's identity, shared with `adb`. |
| service | A room the resident opens on request, such as `shell` or `sync`. |
| stream | One conversation over the cable, kept apart from the others by an id. |
| `shell` | The room that runs commands and returns their output and exit code. |
| `sync` | The room that lists folders and moves files. |
| `zstd`, `lz4`, `brotli` | The three methods for packing a file smaller before it crosses the cable. `zstd` is the default. |
| `shell_v2` | The newer shell form that reports output and the exit code separately. The older form is the fallback. |
| APK | An Android app package, the file that `install` uploads. |
| transport | The byte pipe under the protocol: a USB cable or a network socket. |
| `libusb` | The third-party library that opens the USB cable. Linked dynamically, and optional. |
| `Result<T>` | The library's answer type: a value or an error, never an exception. |
| emulator | A phone simulated on the computer, reached over the network. |
| `tcpip` | A real phone switched to listen over the network instead of the cable. |

## Where to read more

- [`01-objective.md`](01-objective.md) — what the library is for, and what it will not do.
- [`03-roadmap.md`](03-roadmap.md) — the plan, slice by slice, and what is done.
- [`05-usage.md`](05-usage.md) — copy-pasteable code for one feature at a time.
- [`04-blockers.md`](04-blockers.md) — the wall above, and every other trap, in technical terms.
