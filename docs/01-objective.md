# Objective

## Purpose

This repository aims to provide a **simplified, customized reimplementation of ADB (Android Debug Bridge) as a C++ library**. The library is designed to be embedded directly into C++ applications, giving developers programmatic control over Android devices.

## Key Constraint

The library must operate **without depending on Google's ADB server or the `adb` command-line tool**. There is no `adb.exe` process to spawn, no local server on port 5037, and no external binary required. All device communication is handled internally by the library itself.

## Initial Scope

The first iteration targets a minimal but practical feature set:

- **File transfer**: pull and push files to and from a device.
- **Package management**: install and uninstall applications.
- **File listing**: enumerate files and directories on a device.
- **Connection management**: connect to and disconnect from devices.
- **App control**: launch and close applications.
- **App status**: check whether an application is running.

## Non-Goals (for now)

- Full parity with every `adb` subcommand.
- Support for every Android version or device vendor.
- Serving as a drop-in replacement for the official ADB tooling.

## Technical Requirements

- **Cross-platform**: Linux, Windows, and macOS.
- **Language standard**: C++20.
- **Build system**: CMake.
- **Documentation**: Doxygen.

## Guiding Principles

- **Self-contained**: no reliance on external ADB components.
- **Embeddable**: usable as a library from other C++ projects.
- **Focused**: implement only what is needed, cleanly.
- **Portable**: consistent behavior and API across all supported platforms.
