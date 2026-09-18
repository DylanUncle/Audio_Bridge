# Audio Bridge

**English** | [简体中文](README.zh_CN.md)

Audio Bridge is a lightweight, open-source Bluetooth audio playback (A2DP Sink / LE Audio) connector for Windows.

Microsoft added Bluetooth A2DP Sink support in Windows 10 2004, but Windows still ships no built-in UI for managing the connection, so a third-party app is required. Audio Bridge builds on [AudioPlaybackConnector](https://github.com/ysc3839/AudioPlaybackConnector) by Richard Yu: it keeps that project's approach of driving the documented `AudioPlaybackConnection` API from a tray-resident Win32 application, and extends it with automatic reconnection, audio performance tuning, multi-language support and single-installer multi-architecture packaging. See [Acknowledgements](#acknowledgements) for details.

---

## Overview

**What it is.** Audio Bridge is a tray-resident Windows desktop application that lets you connect your PC to Bluetooth audio source devices (phones, tablets, etc.) and receive their audio through Windows. It provides the *control layer*: an enumerable device list, connection lifecycle management, auto-reconnect, notification and an installer that bundles all three CPU architectures.

**What it is not.** Audio Bridge does **not** implement the A2DP / LE Audio protocol stack itself. The actual audio capture, decoding and rendering are performed entirely by Windows' built-in Bluetooth audio services. Audio Bridge only drives the documented Windows `AudioPlaybackConnection` API — a deliberate design decision that keeps the app small, robust and forward-compatible with future Windows versions.

**Key capabilities**

| Capability | Description |
|---|---|
| Dual protocol support | Works with both Classic A2DP and LE Audio devices; shows a `[A2DP]` / `[LE Audio]` prefix in the device picker |
| Smart auto reconnect | Exponential backoff + jitter reconnection scheduling that reduces multi-device reconnection conflicts |
| Audio performance boost | Applies MMCSS "Pro Audio" scheduling, a system execution-state lock and process priority boost while a connection is active |
| Multi-device memory | Remembers multiple paired devices for quick switching; legacy configuration is migrated automatically |
| Auto start on boot | Optional launch on Windows startup (registry first, Startup shortcut as fallback) |
| Windows notifications | Toast notifications when a device connects or disconnects |
| System tray integration | Runs in the notification area with a native device picker |
| Multi-language UI | English, Simplified Chinese and Traditional Chinese (follows the system UI language) |
| Single installer, three architectures | One setup package contains x86, x64 and ARM64 binaries and installs the matching one |

**Technology stack.** Win32 + C++20, C++/WinRT for the Windows Runtime APIs, WIL for error handling and RAII, MSBuild/v143, Inno Setup for packaging, and a gettext PO → binary YMO localization pipeline.

---

## Operation Manual

### 1. Install

1. Download and run `Audio_Bridge_Setup.exe`.
2. Follow the wizard. On the **Additional Icons** step you may optionally enable:
   - **Create a desktop icon**
   - **Start on Boot** (launches Audio Bridge automatically at logon)
3. The installer detects your CPU architecture (x86 / x64 / ARM64) and installs the matching binary as `Audio_Bridge.exe` under `%ProgramFiles%\Audio_Bridge` (or the per-user equivalent, since the installer requests the lowest privilege level).

> The setup package contains all three architecture binaries; only the one matching your system is installed.

### 2. First run

1. Launch **Audio Bridge** from the Start menu. The app appears only as a **tray icon** in the notification area — there is no main window.
2. On first launch the app loads its configuration from `%LOCALAPPDATA%\Audio_Bridge\Audio_Bridge.json`. If that file does not exist yet, defaults are used and the auto-start state is inferred from the system (registry / Startup folder).

### 3. Pair a device

Before Audio Bridge can connect, the Bluetooth device must be **paired** with Windows. Audio Bridge does not perform pairing.

1. Right-click the tray icon and choose **Bluetooth Settings**, or open Windows Settings → Bluetooth & devices.
2. Add / pair your phone or tablet as usual.

### 4. Connect a device

1. **Left-click** the tray icon (or right-click → the picker opens). A native device picker lists the Bluetooth audio source devices that support `AudioPlaybackConnection`.
2. Each entry is prefixed with its detected protocol, e.g. `[A2DP] Pixel 8`.
3. Select a device to connect. The picker status text reports progress (`Connecting…` → `Connected`, or `Retry` on failure).
4. Audio from the device now plays through your PC's default output device.

To disconnect, use the **Disconnect** button in the picker, or disconnect from the phone side. Connections opened by other (non-UWP) paths can only be closed by the system; see *Technical Notes* below.

### 5. Tray menu reference

Right-click the tray icon to open the menu:

| Item | Action |
|---|---|
| (device picker) | Left-click opens the picker at the cursor |
| **Bluetooth Settings** | Opens the Windows Bluetooth settings page |
| **Reconnect** | Toggles automatic reconnection (checkmark reflects the saved setting) |
| **Start on Boot** | Toggles launch-at-logon (checkmark reflects the saved setting) |
| **Statistics** | Shows the connection statistics panel |
| **Exit** | Quits the app (asks for confirmation while a connection is active) |

### 6. Configuration

Settings are stored as JSON at:

```
%LOCALAPPDATA%\Audio_Bridge\Audio_Bridge.json
```

| Key | Type | Meaning |
|---|---|---|
| `reconnect` | boolean | Enable automatic reconnection (default `true`) |
| `autoStart` | boolean | Launch on Windows startup |
| `lastDevices` | string[] | Remembered device IDs, used for auto reconnect |

Notes:

- The file is written **atomically** (temp file → flush → atomic replace) so a crash or power loss cannot corrupt your settings.
- On first run, a legacy configuration located next to the old executable (same file name) is imported automatically. The old file is kept as a backup.
- `Start on Boot` is implemented with the `HKCU\...\CurrentVersion\Run` registry value `Audio_Bridge`; if the registry write fails, the app falls back to a shortcut in the Startup folder.

### 7. Notifications

- A toast notification is shown when a device connects, and when a device disconnects.
- Disconnect toasts are throttled (per device) to avoid notification spam when a connection flaps.
- When auto-reconnect gives up after too many attempts, an "Auto-reconnect stopped" notification is shown.
- Toast headers use the app's icon, which requires the Start-menu shortcut's AppUserModelID (`AudioBridge`) to match the one used by the app. The installer creates this shortcut for you.

### 8. Uninstall

Use **Settings → Apps → Installed apps → Audio Bridge → Uninstall**, or the Start-menu uninstall entry.

The uninstaller first asks the running tray app to exit gracefully (via a private `WM_APP_QUITREQUEST` message), then force-terminates it if it does not exit in time. If the process still cannot be terminated — a rare Bluetooth-driver hang that pins it in the kernel — the uninstall is **not** aborted: the locked `Audio_Bridge.exe` is renamed aside so the remaining files can be removed normally, and a logon cleanup task is registered in `RunOnce` — a self-deleting VBScript run by `wscript.exe`, which, being a GUI-subsystem host launched with `//B`, stays completely silent and never flashes a console window — to delete the leftover automatically after the next logon. You get an informational message, never a failed uninstall that deletes nothing.

### 9. Troubleshooting

| Symptom | Explanation / fix |
|---|---|
| The app shows as `svchost.exe` in the Volume Mixer or SteelSeries Sonar | Expected platform behaviour — see [About svchost.exe](#about-svchostexe-a-platform-limitation-you-should-know) |
| Device does not appear in the picker | The device must be paired and must support A2DP Sink / LE Audio; re-pair it in Windows Bluetooth settings |
| Second launch does nothing | Audio Bridge is single-instance; launching it again just opens the device picker of the running instance |
| Auto-reconnect keeps failing | After the retry budget is exhausted the app stops trying for that device and notifies you; reconnect manually or re-pair |
| No icon in the tray after an Explorer restart | The app listens for the `TaskbarCreated` message and re-adds the icon automatically; if it still fails, restart the app |

---

# Features
- **Dual Audio Protocol Support**: Supports both A2DP and LE Audio (Low Energy Audio) with automatic protocol negotiation, and shows a `[A2DP]` / `[LE Audio]` prefix in the device picker
- **Smart Auto Reconnect**: Automatically reconnects with exponential backoff + jitter scheduling (based on the BLEdge algorithm) to reduce multi-device reconnection conflicts
- **Audio Performance Boost**: Applies MMCSS "Pro Audio" scheduling, system execution-state lock and process priority boost while connections are active, reducing audio stutter
- **Auto Start on Boot**: Option to start the program automatically when Windows starts (registry first, Startup shortcut as fallback)
- **Multi-Device Memory**: Remember multiple paired devices for quick switching; legacy config is migrated automatically
- **Windows Notifications**: Receive toast notifications when a device connects or disconnects
- **System Tray Integration**: Minimize to system tray; the tray icon uses the application's built-in icon resource
- **UWP Device Picker**: Modern device selection interface powered by Windows UWP APIs
- **Multi-language UI**: English, Simplified Chinese and Traditional Chinese (detected from system UI language)

## Audio Protocol Support
This application leverages the Windows `AudioPlaybackConnection` API, which automatically handles audio protocol negotiation:

- **A2DP**: Compatible with most Bluetooth devices, widely supported on all smartphones
- **LE Audio**: Lower latency, lower power consumption, better audio quality (requires Bluetooth 5.2+ hardware, device support, and Windows 11 22H2+)

Protocol selection is handled automatically by the Windows Bluetooth stack. The application does not implement explicit protocol selection logic. The `[A2DP]` / `[LE Audio]` prefix is purely informational and is derived from the device's `System.Devices.Aep.ProtocolId` property.

## About svchost.exe: A platform limitation you should know

### Why does it show as svchost.exe in Volume Mixer / SteelSeries Sonar?

This is **not a bug in this application** — it is determined by the architecture of Windows' `AudioPlaybackConnection` API.

This application only handles the "control layer": managing device lists, establishing Bluetooth connections, and providing a tray UI. The actual audio data — receiving the A2DP stream from your phone, decoding it, and rendering it to your speakers — is all handled internally by Windows' Bluetooth audio services, which run inside `svchost.exe` (specifically the `BTAGService` Bluetooth Audio Gateway Service and `Audiosrv` Windows Audio Service).

As a result:
- The **Windows Volume Mixer (sndvol)** shows `svchost.exe` as the producing process, not Audio Bridge
- **SteelSeries Sonar** classifies it as svchost.exe by process name, and because svchost runs under the `LocalService` system identity, Sonar shows "Sonar is not allowed to change audio settings"
- Sonar heuristically assigns svchost to the "Game" or other categories rather than the ones you might expect

This is a **shared limitation of all applications using the `AudioPlaybackConnection` API** — including Microsoft's own sample code and similar apps on the UWP Store. There is currently no workaround at the application level.

### Workarounds for Sonar users

While you cannot rename or re-categorize svchost within Sonar, you can achieve the same effect through the system layer:

**Option 1: Change the default audio output device in Windows (recommended)**

1. Open Windows Settings → System → Sound
2. Under "Output", select **SteelSeries Sonar - Media** (or whichever Sonar virtual device you want Bluetooth audio to route through) as the default
3. Connect your phone and start playing — audio will automatically flow through Sonar's Media virtual device, and all your Sonar EQ, spatial audio, and presets will apply

**Option 2: Use Windows Volume Mixer directly**

Open the Volume Mixer (Win+R → type `sndvol`). The svchost.exe audio session can be freely adjusted here — volume, mute, and all standard controls work fine and are not restricted by Sonar.

**Option 3: If your device supports LE Audio (Bluetooth 5.2+)**

Some LE Audio connections take a different system path and are occasionally identified more accurately by Sonar. If your device supports both A2DP and LE Audio, try switching between them.

### Can we make it show as "Audio Bridge"?

Theoretically, you could enumerate svchost's audio sessions via `IAudioSessionManager2` and call `IAudioSession::SetDisplayName` to "relabel" them. However, this API has two hard blockers:

1. **Permission isolation**: Audiosrv / BTAGService inside svchost run under `NT AUTHORITY\LocalService` or `NetworkService`. Calling `SetDisplayName` from a regular user process will return `E_ACCESSDENIED`.
2. **Sonar ignores it**: Sonar's process classification logic looks at the **executable filename** (svchost.exe), not the session's display name.

So this road is currently blocked. It's a known platform limitation — we're waiting for Microsoft to expose richer audio-session control through the `AudioPlaybackConnection` API in the future.

# Supported Architectures
A single setup package contains all three architecture binaries and installs the matching one automatically:

| Architecture | Build output | Typical devices |
|---|---|---|
| x86 (32-bit) | `Release\Audio_Bridge32.exe` | Legacy 32-bit Windows |
| x64 (64-bit) | `x64\Release\Audio_Bridge64.exe` | Standard x64 PCs |
| ARM64 | `ARM64\Release\Audio_BridgeARM64.exe` | Snapdragon / Surface Pro X |

After installation, all variants are unified to `Audio_Bridge.exe`; the installed architecture is recorded in `{app}\installed.arch.txt` for troubleshooting.

# Configuration
Settings are stored in a JSON file at:
```
%LOCALAPPDATA%\Audio_Bridge\Audio_Bridge.json
```
Available options: `reconnect` (auto reconnect), `autoStart` (start on boot), `lastDevices` (remembered device IDs). On first run, a legacy config located next to the old executable is imported automatically.

# Internationalization
The UI language follows the system UI language. Translations are maintained as gettext PO files under `translate/source/` and compiled into a compact binary resource (YMO, FNV-1a 32-bit hash lookup, similar to gettext MO) at build time:

1. `translate/gen_pot.sh` — extract strings from source into `messages.pot`
2. Translate `zh_CN.po` / `zh_TW.po` (e.g. with Poedit)
3. `translate/gen_rc.sh` — convert PO files to YMO and generate `translate/generated/translate.rc`

At runtime the app loads the YMO resource whose language matches the current thread UI language (`FindResourceExW(..., GetThreadUILanguage())`). Strings without a translation fall back to the original English text.

# Building from Source

## Prerequisites
- Visual Studio 2022 with the **Desktop development with C++** workload (v143 toolset)
- Windows SDK 10.0.26100.0 (or newer)
- [Inno Setup 6](https://jrsoftware.org/isinfo.php) (for the installer)
- Dependencies are restored via NuGet (`Microsoft.Windows.CppWinRT`, `Microsoft.Windows.ImplementationLibrary`)

## Build
1. Open `AudioBridge.sln` in Visual Studio.
2. Build the **Release** configuration for the three platforms: `Release|x86` (Win32), `Release|x64`, `Release|ARM64`.
3. Copy the three executables into the root `Release\` folder:
   - `Release\Audio_Bridge32.exe`
   - `Release\Audio_Bridge64.exe`
   - `Release\Audio_BridgeARM64.exe`
4. Open `setup.iss` in Inno Setup and compile — the installer is produced at `Output\Audio_Bridge_Setup.exe`.

You can also build all three architectures from the command line:

```powershell
$msb = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe'
foreach ($p in 'x86','x64','ARM64') {
    & $msb AudioBridge.sln /p:Configuration=Release /p:Platform=$p /m /v:minimal
}
```

# Implementation & Technical Path

This section describes how the application is actually built, from process startup to the audio-control APIs.

## Architecture at a glance

```
Audio_Bridge.exe
  ├── Win32 shell layer        AudioBridge.cpp / AudioBridge.h  (wWinMain, window proc, tray, menu, timers)
  ├── Application façade       AudioPlaybackApp.hpp             (Meyers singleton: state + orchestration)
  ├── Connection layer         ConnectionManager.hpp            (thread-safe connection table, ConnectAsync)
  │     ├── ReconnectScheduler.hpp                              (backoff + jitter, circuit breaker)
  │     └── AudioPerformanceBoost.hpp                           (MMCSS "Pro Audio" thread, ref-counted)
  ├── Device monitoring        AudioDeviceMonitor.hpp           (IMMNotificationClient: endpoint vanish)
  ├── Persistence              SettingsUtil.hpp                 (JSON config, atomic write, auto-start)
  ├── Localization             I18n.hpp / FnvHash.hpp           (YMO lookup + FNV-1a hash)
  └── Utilities                Util.hpp                         (UTF-8/16 conversion, module paths)
```

The code is deliberately split so that each concern is a self-contained header where possible, and so that the Win32 details never leak into the connection logic.

## 1. Startup, window and message loop

1. `wWinMain` sets the process AppUserModelID to `AudioBridge` (required for toast notifications), initializes a **single-threaded apartment** (`winrt::init_apartment`), and acquires a single-instance mutex named `AudioBridge_SingleInstance`.
   - If another instance is already running, the new process locates the existing hidden window by class name `AudioBridge` and posts a private message that opens the device picker, then exits.
2. It registers a window class named `AudioBridge` (icon loaded from the compiled `.ico` resource) and creates a **hidden host window** (`WS_EX_TOOLWINDOW`). All shell integration hangs off this window.
3. Initialization order: load translation resource → load settings → apply auto-start → build menu → create the WinRT device picker → create the tray icon.
4. The tray icon is registered through `Shell_NotifyIcon` (`NIM_ADD` / `NIM_MODIFY` / `NIM_SETVERSION`, `NOTIFYICON_VERSION_4`). It also listens for the `TaskbarCreated` message so the icon reappears automatically after an Explorer restart.
5. The message loop is a standard `GetMessageW` / `TranslateMessage` / `DispatchMessageW` pump.

**Threading rule.** All window-affine operations (timer arming, tray updates, window destruction) are marshalled back to the main thread. Background work (WinRT connection coroutines, device-state callbacks) never touches the window directly; it posts messages instead. This is a core correctness invariant of the design.

## 2. Device enumeration and connection

- The picker is a WinRT `Windows.Devices.Enumeration.DevicePicker`, bound to the hidden window via `IInitializeWithWindow`. Its filter is `AudioPlaybackConnection::GetDeviceSelector()`, so it only lists devices that the Windows API can actually connect to.
- Connecting is done in `ConnectionManager::ConnectAsync` and follows this path:
  1. **Idempotency + in-flight guard** — if the device is already connected, return immediately; if a connection to the same device is already in progress, ignore the duplicate request.
  2. Create the connection object with `AudioPlaybackConnection::TryCreateFromId(deviceId)`.
  3. Subscribe to `StateChanged` to detect closure (including closures caused by the phone side).
  4. `co_await connection.StartAsync()` and `co_await connection.OpenAsync()`, each wrapped in a **45-second timeout** (`WithTimeout`) that is deliberately longer than the protocol layer's own 20–30 s timeout.
  5. On success: record statistics, update the picker status, acquire a reference on the performance boost, cancel any pending reconnect for that device, and raise the connection event (which drives the toast).
  6. On failure: classify the failure (`RequestTimedOut` / `DeniedBySystem` / `UnknownFailure`), clean up the entry and set the picker to `Retry`.
- Cancellation is cooperative: the map entry is inserted *before* the awaits so that a shutdown request can always close the in-progress connection.

**Why the redundancy?** Connections created through the desktop (non-UWP) path generally **cannot** be closed programmatically — calling `Close()` returns Access Denied. Disconnection is therefore detected asynchronously through three independent mechanisms: the `StateChanged` event, a 30-second health-check timer, and default-endpoint notifications (see below).

## 3. Protocol detection (A2DP vs LE Audio)

Detection is informational only. `DetectProtocol` reads the device property `System.Devices.Aep.ProtocolId` and compares it against the Bluetooth LE Audio protocol GUID `{0BB58923-2600-489B-9FC9-DE57A5669D6F}`:

- GUID equals the BLE Audio GUID → `LEAudio`
- property present but different → `ClassicA2DP`
- property missing / unreadable → `Unknown`

The result becomes the `[LE Audio]` / `[A2DP]` prefix shown in the picker and statistics. The real protocol negotiation is performed entirely by the Windows Bluetooth stack.

## 4. Auto-reconnect scheduler (exponential backoff + jitter)

`ReconnectScheduler` implements the retry policy used after an unexpected disconnect:

- Base delay `5000 ms × 2^min(retryCount, 6)`, capped at `300000 ms` — giving the sequence 5 / 10 / 20 / 40 / 80 / 160 / 300 s.
- **±20 % random jitter** is applied to every delay so that multiple devices reconnecting at the same time do not collide (the approach is adapted from the *BLEdge* scheduling algorithm).
- A floor of `1000 ms` prevents near-zero delays.
- After **20 attempts** the scheduler trips a circuit breaker for that device, removes it from the queue and raises an "Auto-reconnect stopped" notification.

Timing is driven by a **single Win32 timer** (ID 1). The timer is re-armed from the schedule's next due time; because `SetTimer` must be called from the thread that owns the window, cross-thread events simply post a re-arm message to the main thread.

## 5. Audio performance boost (MMCSS)

While at least one connection is active, `AudioPerformanceBoost` raises the audio pipeline's scheduling priority:

- It keeps the **process** at `NORMAL_PRIORITY_CLASS` and confines all boosting to **one dedicated thread**, guaranteeing that `AvSetMmThreadCharacteristics` and `AvRevertMmThreadCharacteristics` are called on the same thread (a documented requirement).
- The boost thread: registers with MMCSS under the **"Pro Audio"** task, sets `AVRT_PRIORITY_HIGH`, and holds `SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_AWAYMODE_REQUIRED)`.
- `ES_DISPLAY_REQUIRED` is intentionally **omitted** so the app never forces the screen to stay on.
- The boost is **reference-counted**: it starts when the first connection opens and stops when the last one closes. The thread waits on an event with zero CPU cost while active.

## 6. Endpoint monitoring and health checks

- `AudioDeviceMonitor` implements `IMMNotificationClient` and listens for default-audio-endpoint changes and render-endpoint removal. If the endpoint a Bluetooth connection was rendering to disappears (e.g. a USB headset is unplugged), the app treats the connection as gone and cleans up.
- A 30-second timer (`IDT_HEALTHCHECK`) periodically reconciles the internal connection table with reality, covering the cases the event-based paths cannot observe.

## 7. Settings and configuration store

- Path: `%LOCALAPPDATA%\Audio_Bridge\Audio_Bridge.json` (the directory is created on demand).
- Writes are **atomic**: a `*.tmp` file is written, flushed with `FlushFileBuffers`, then swapped in with `MoveFileExW(MOVEFILE_REPLACE_EXISTING)`. A partial write can never leave a truncated config behind.
- Legacy migration: on first run, a config next to the old executable is imported into the new location and kept as a backup.
- Auto-start is applied on two channels: the registry Run value (preferred) and a Startup-folder shortcut (fallback). The two are kept mutually exclusive to avoid duplicate launch entries.

## 8. Localization pipeline

- Source strings use the `_(L"...")` macro. At build time `gen_pot.sh` extracts them with `xgettext`, and `gen_rc.sh` compiles the translated `zh_CN.po` / `zh_TW.po` into binary **YMO** resources.
- The YMO format is a compact hash table: a 16-bit entry count, then `(uint32 hash, uint16 offset)` pairs, then the UTF-16 translation data. The hash is **FNV-1a 32-bit** over the UTF-16LE bytes of the source string — which matches the in-memory representation used by the C++ lookup, so no conversion is needed at runtime.
- At startup `LoadTranslateData` selects the resource for the current thread UI language; `Translate()` then does a hash lookup with a pointer cache for hot strings. Unmatched strings return the original English.

## 9. Build and packaging

- The project builds three Release targets (`x86`/Win32, `x64`, `ARM64`) with the v143 toolset, C++20 (`/std:c++latest`), `/utf-8`, and **warnings-as-errors** at level 4.
- Output binaries are named by the `TargetName` property: `Audio_Bridge32.exe`, `Audio_Bridge64.exe`, `Audio_BridgeARM64.exe`.
- `setup.iss` (Inno Setup) packages all three binaries into a single `Audio_Bridge_Setup.exe`, installs only the matching architecture as `Audio_Bridge.exe`, and registers the AppUserModelID, Start-menu / desktop / startup shortcuts and uninstall entries.
- Because the app is a tray-resident process that holds its own executable open, the installer and uninstaller stop it through a private `WM_APP_QUITREQUEST` window message, wait for a graceful exit, and fall back to `taskkill /F`. If the process ends up pinned in the kernel (a rare Bluetooth-driver hang, where the process survives `taskkill /F`), nothing is aborted. The locked `Audio_Bridge.exe` is renamed to `.old` — a loaded image cannot be deleted but it can still be renamed, which frees the original path so the rest of the uninstall proceeds — and a logon cleanup task registered in `RunOnce` at that moment removes the leftover after the next logon. It is a self-deleting VBScript driven by `wscript.exe //B`; because `wscript.exe` is a GUI-subsystem host, the cleanup is fully silent and does not flash a console window at logon the way a `cmd /C del` entry would. (`MOVEFILE_DELAY_UNTIL_REBOOT` is not usable here: it needs write access to `HKLM\...\Session Manager\PendingFileRenameOperations`, and the installer runs with `PrivilegesRequired=lowest`.) The same rename-aside path protects installs and upgrades.

# Project Structure
| File | Responsibility |
|---|---|
| `AudioBridge.cpp` / `AudioBridge.h` | Entry point, hidden host window, message loop, tray icon and menu, timers, notification messages |
| `AudioPlaybackApp.hpp` | Application root (Meyers' singleton façade): shared state, orchestration, graceful shutdown, connection entry points |
| `ConnectionManager.hpp` | Thread-safe connection table, cancellable `ConnectAsync`, protocol detection (A2DP / LE Audio), connection events and statistics |
| `ReconnectScheduler.hpp` | Exponential backoff + jitter reconnection scheduler with circuit breaker |
| `AudioPerformanceBoost.hpp` | MMCSS "Pro Audio" scheduling + execution-state lock (RAII, ref-counted, dedicated thread) |
| `AudioDeviceMonitor.hpp` | `IMMNotificationClient` implementation: default-endpoint change / endpoint removal notifications |
| `SettingsUtil.hpp` | Config storage at `%LOCALAPPDATA%`, atomic writes, legacy config migration, auto-start (registry + Startup shortcut fallback) |
| `I18n.hpp` / `FnvHash.hpp` | Localization (YMO resource lookup, FNV-1a 32-bit hash) |
| `Util.hpp` | UTF-8/UTF-16 conversion, module path helpers |
| `AudioBridge.rc` / `resource.h` | Windows resources: application icon, version info, embedded SVG |
| `AudioBridge.manifest` | Application manifest (DPI awareness, supported OS, common controls) |
| `AudioBridge.ico` / `Audio_Bridge_Icon.png` | Compiled application icon and its PNG source artwork |
| `translate/` | gettext PO sources and the YMO build tooling (`po2ymo.py`, `gen_pot.sh`, `gen_rc.sh`) |
| `setup.iss` | Inno Setup script packaging all three architectures |
| `clean.cmd` | Removes MSBuild intermediate/debug directories without touching Release binaries |

# System Requirements
- **Windows 10, version 2004 (build 19041) or later** — the baseline for A2DP Sink playback (the `AudioPlaybackConnection` API is available from this version)
- **LE Audio is an optional enhancement**, additionally requiring Windows 11, version 22H2 (build 22621) or later, a Bluetooth 5.2+ adapter, and a LE Audio capable device
- Bluetooth adapter with A2DP Sink support
- Any of the supported CPU architectures: x86, x64, or ARM64

# Acknowledgements
Audio Bridge is a derivative work of [AudioPlaybackConnector](https://github.com/ysc3839/AudioPlaybackConnector) by Richard Yu ([@ysc3839](https://github.com/ysc3839)), which is released under the MIT License. The original project established the approach this app is built on — driving the documented Windows `AudioPlaybackConnection` API from a tray-resident Win32 application. Audio Bridge continues that line of work and adds automatic reconnection with exponential backoff and jitter, audio performance tuning (MMCSS), multi-device memory, auto-start handling, and this documentation.

The localization toolchain under `translate/` is likewise based on the same author's [translate](https://github.com/ysc3839/translate) fork of translate-toolkit, and is used at build time only.

Thanks are also due to the open-source components this project builds upon:

| Component | License | Role |
|---|---|---|
| [Windows Implementation Library (WIL)](https://github.com/microsoft/wil) | MIT | Error handling and RAII wrappers |
| [C++/WinRT](https://github.com/microsoft/cppwinrt) | MIT | Windows Runtime API projections |
| [Inno Setup](https://jrsoftware.org/isinfo.php) | Own license | Installer packaging (build time only) |
| GNU gettext (`xgettext`) | GPL | String extraction (build time only) |

> Inno Setup and gettext are invoked only as build-time tools and no code from them is linked into the distributed binaries, so they do not affect the MIT licensing of this project's own output.

# License
MIT License - See [LICENSE](LICENSE) for details.
