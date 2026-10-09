# AudioPlaybackConnector for Windows 11

**English** | [简体中文](README.zh_CN.md)

[![Latest release](https://img.shields.io/github/v/release/summerwind6/AudioPlaybackConnector-Win11?label=release)](https://github.com/summerwind6/AudioPlaybackConnector-Win11/releases/latest)
[![Build](https://img.shields.io/github/actions/workflow/status/summerwind6/AudioPlaybackConnector-Win11/build.yaml?branch=win11-compatibility&label=build)](https://github.com/summerwind6/AudioPlaybackConnector-Win11/actions/workflows/build.yaml)
[![License](https://img.shields.io/github/license/summerwind6/AudioPlaybackConnector-Win11)](LICENSE)
[![Windows](https://img.shields.io/badge/Windows-10%202004%2B%20%7C%2011-0078D4?logo=windows11)](https://github.com/summerwind6/AudioPlaybackConnector-Win11/releases/latest)

Turn a Windows 10/11 PC into a Bluetooth speaker. This app enables the built-in Windows **Bluetooth A2DP Sink**, allowing audio from an Android phone or another paired Bluetooth audio source to play through the PC.

> This is a Windows 11 compatibility fork of [ysc3839/AudioPlaybackConnector](https://github.com/ysc3839/AudioPlaybackConnector). Credit for the original project belongs to its author.

## Highlights

- Windows 11-style Bluetooth device picker
- Connect and disconnect paired phones from the notification area
- Optional automatic reconnection on the next launch
- Single-instance operation with support for multiple connected source devices
- Independent PC-side playback volume control
- Improved A2DP connection lifecycle for Windows 11
- Portable single-file executable with no installer
- Official x86, x64 and ARM64 builds
- Simplified Chinese, Traditional Chinese and English interface

## Requirements

- Windows 10 version 2004 (build 19041) or later, or Windows 11
- A working Bluetooth adapter and up-to-date Bluetooth driver
- A phone or other Bluetooth audio source already paired in Windows Settings

## Download

Download the latest version from [GitHub Releases](https://github.com/summerwind6/AudioPlaybackConnector-Win11/releases/latest).

| File | System architecture | Recommended for |
| --- | --- | --- |
| `AudioPlaybackConnector64.exe` | x64 / AMD64 | Most Intel and AMD Windows PCs |
| `AudioPlaybackConnector32.exe` | x86 | 32-bit Windows systems |
| `AudioPlaybackConnectorARM64.exe` | ARM64 | Windows on ARM devices, such as Snapdragon PCs |

SHA-256 checksums are included in `SHA256SUMS.txt` on the Release page.

## Usage

1. Pair the phone with the PC in **Settings → Bluetooth & devices**.
2. Run the executable matching the PC architecture. The app appears in the notification area.
3. Click the AudioPlaybackConnector icon.
4. Select the paired phone and click **Connect**.
5. Play audio on the phone. Sound is routed to the current Windows output device.

Right-click the notification-area icon to open Bluetooth settings or exit the app. When exiting, **Reconnect on next start** can remember active devices and reconnect them on the next launch.

The volume slider controls this app's Windows audio session independently from the phone volume and the system speaker volume. The effective output is the combination of all three levels.

Settings and diagnostic logs are stored under `%LOCALAPPDATA%\AudioPlaybackConnector`, not beside the executable. An existing `AudioPlaybackConnector.json` beside the executable is migrated automatically.

English, Simplified Chinese and Traditional Chinese are selected automatically from the Windows display language.

## Troubleshooting

### Connected, but there is no audio

On the first connection to each device, the app starts the Windows A2DP sink and briefly waits for the Bluetooth service to stabilize before opening the connection. It opens the connection only once, so a phone that is already playing is not interrupted by an automatic disconnect/reconnect cycle. If a device still connects without audio, disconnect it in AudioPlaybackConnector, wait two seconds, then connect it once more.

### Connection error, or still no audio after reconnecting

1. Fully exit AudioPlaybackConnector.
2. Turn the PC's Bluetooth off and back on.
3. Start AudioPlaybackConnector and connect the phone again.

Removing and pairing the phone again is usually unnecessary.

### No compatible devices are listed

Confirm that the phone is already paired in Windows Bluetooth settings, then close and reopen the device picker.

## Build from source

Install Visual Studio 2022 or later with:

- Desktop development with C++
- MSVC v143 build tools
- C++ ATL for the target architecture
- Windows 11 SDK
- NuGet

Restore NuGet packages, then build `AudioPlaybackConnector.sln` in the `Release` configuration for `Win32`, `x64` or `ARM64`.

## Windows 11 changes

- Replaced the legacy system device picker with a custom Windows 11-style picker
- Improved device discovery and connected-state refresh
- Added safer A2DP connection cleanup and shutdown handling
- Added automated x86, x64 and ARM64 Release builds
- Added clearer connection errors and recovery guidance

## Technical notes

The app uses C++/WinRT and the Windows `AudioPlaybackConnection` API. Bluetooth codec negotiation and audio decoding are handled by the Windows Bluetooth stack; the app does not select SBC, AAC, aptX or LDAC directly.

## Connection reliability

Windows can occasionally report an A2DP sink connection as open before its audio transport is ready, especially on the first connection after startup. This build briefly warms up the sink service before the first open and enforces a short release delay before a manual rapid reconnection. It does not automatically close a successful connection, which preserves an A2DP stream that was already playing on the phone. If audio is still silent after that recovery, restart the PC's Bluetooth as described above.

## License and acknowledgments

Released under the [MIT License](LICENSE).

- Original project: [ysc3839/AudioPlaybackConnector](https://github.com/ysc3839/AudioPlaybackConnector)
- Windows 11 compatibility fork: [summerwind6/AudioPlaybackConnector-Win11](https://github.com/summerwind6/AudioPlaybackConnector-Win11)
