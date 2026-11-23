# PadSplit

**Play single-player games fullscreen with a gamepad on Monitor 1 while freely using your mouse and keyboard on Monitor 2** — no minimizing, no muting, no pausing, and no accidental camera spins or keystroke leaks.

[![Build & Test](https://github.com/theman6660/PadSplit/actions/workflows/build.yml/badge.svg)](https://github.com/theman6660/PadSplit/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Windows%20x64%20%2F%20x86-0078D4.svg)](#)

**English** | [简体中文](README_CN.md)

---

## Why PadSplit?

Windows only allows a single foreground window at a time. On a dual-monitor setup, clicking a browser, IDE, chat app, or AI agent on your second screen causes most PC games to:
1. Lock or warp your cursor back to the game monitor (`ClipCursor`, `SetCursorPos`, `RIDEV_CAPTUREMOUSE`, `SetCapture`),
2. Pause, mute audio, throttle FPS, or drop controller input on focus loss (`WM_KILLFOCUS`, `WM_ACTIVATEAPP`, `GetForegroundWindow`), or
3. Minimize out of fullscreen.

**PadSplit** solves this cleanly inside the game process for offline single-player games — zero virtual machines, zero desktop virtualization, and native Windows experience (IME, apps, drag-and-drop) on Monitor 2.

```text
+-----------------------------------+     +-----------------------------------+
|  Monitor 1 (Borderless Game)      |     |  Monitor 2 (Any Native Windows)   |
|                                   |     |                                   |
|  [Game Process + padsplit_core]   |     |  Browser / IDE / AI Agent / Chat  |
|   * Focus spoofed as foreground   |     |   * Full mouse & keyboard focus   |
|   * Gamepad active in background  |     |   * Native clicks & typing        |
|   * KBM isolated from game        |     |   * Zero cursor lock or warp      |
|   * Audio & FPS uninterrupted     |     |                                   |
+-----------------------------------+     +-----------------------------------+
               ^                                           ^
           [Gamepad]                               [Mouse & Keyboard]
```

## Key Features

- **Native x64 & x86 Game Support**: Ships both `padsplit_core_x64.dll` and `padsplit_core_x86.dll` (via a tiny 32-bit injector helper) with **zero external runtime DLL dependencies**.
- **Focus Spoofing**: Intercepts `WM_KILLFOCUS`, `WM_ACTIVATE`, `WM_ACTIVATEAPP`, `WM_NCACTIVATE`, `PeekMessage`, and `GetForegroundWindow` / `GetActiveWindow` / `GetFocus` so the game stays unpaused, unmuted, and at full framerate.
- **Cursor Liberation & KBM Isolation**:
  - Releases `ClipCursor`, blocks `SetCursorPos` / `SetCapture`, and strips `RIDEV_CAPTUREMOUSE` / `RIDEV_NOLEGACY` / `RIDEV_NOHOTKEYS` so Monitor 2 remains 100% clickable and typeable.
  - **Gamepad-Only mode (default)**: Game ignores all mouse/keyboard input while attached — ideal for FPS and 3D action games (no camera whips).
  - **Border-Crossing mode (optional)**: Mouse & keyboard work normally when the cursor is on the game monitor, and isolate automatically when moving to Monitor 2 — ideal for 2D / strategy games.
  - **Idle Cursor Auto-Hide**: Automatically hides a stationary cursor on the game monitor after a configurable delay (default `1500 ms`).
- **Full Gamepad Background Keep-Alive**:
  - **XInput** (`xinput1_4`, `xinput1_3`, `xinput9_1_0` — blocks `XInputEnable(FALSE)`)
  - **DirectInput 8** (rewrites `DISCL_FOREGROUND | DISCL_EXCLUSIVE` to `DISCL_BACKGROUND | DISCL_NONEXCLUSIVE` and auto-reacquires)
  - **Raw Input** (automatically appends `RIDEV_INPUTSINK` for gamepad/joystick HID usages)
  - **Microsoft GameInput (v1–v4)** (enables background input policy and filters KBM readings while preserving gamepad)
  - **DirectSound** (`DSBCAPS_GLOBALFOCUS` fallback for older titles that mute on focus loss)
- **Layered Borderless Fullscreen**: Zero-rendering-overhead window style strip by default (safe with ReShade / RTSS / DLSS overlays), plus an optional `force-dx` mode hooking DXGI `SetFullscreenState` & D3D9 `CreateDevice`/`Reset` for legacy exclusive-fullscreen games.
- **Built-in Anti-Cheat Guard**: Scans target executable, loaded modules, active anti-cheat processes, game directories, and kernel drivers (`Randgrid.sys`, `EasyAntiCheat`, `BEDaisy`, `vgk.sys`, etc.) and **refuses to inject** into protected titles.
- **Bilingual Tray UI + Full CLI**: Silent system-tray daemon (0% GPU when panel is closed), auto-detects English / 简体中文 (with manual toggle), whitelist auto-attach, and a scriptable `padsplit_cli.exe` with JSON telemetry and live log streaming.

---

## Quick Start

### Option 1: Download Prebuilt Release
1. Grab the latest `PadSplit-v*-win-x64.zip` from the [Releases](https://github.com/theman6660/PadSplit/releases) page and extract it anywhere (portable — no installer needed).
2. Run `PadSplit.exe`:
   - It starts silently in the system tray (double-click the tray icon or run `PadSplit.exe` again to open the control panel).
   - Optionally enable **"Launch minimized to system tray on Windows startup"** in Settings.
3. Launch any offline single-player game and press **`Ctrl + Alt + P`** to attach (automatically saved to your whitelist for future launches).
4. Press **`Ctrl + Alt + G`** anytime to temporarily pause/resume KBM isolation and hand the mouse back to the game.

### Option 2: Build from Source

Requires Python 3.10+, Git, and [Ninja](https://ninja-build.org/) (`pip install ninja`).

```powershell
git clone https://github.com/theman6660/PadSplit.git
cd PadSplit

# Downloads pinned portable llvm-mingw, MinHook, Dear ImGui, and nlohmann/json
python scripts/setup_deps.py

# Builds all x64 & x86 binaries into dist/
python build.py

# Runs DLL import audit, anti-cheat checks, x64/x86 mock-game E2E tests, and GUI smoke test
python scripts/run_tests.py

# (Optional) Creates portable release zip + SHA256SUMS.txt in build/release/
python scripts/package_release.py
```

---

## Important Notes & Troubleshooting

### Single-Player Games Only (Anti-Cheat Policy)
PadSplit is strictly designed for **offline single-player games**. It uses standard user-mode DLL injection and API hooking (`MinHook`), and includes a built-in safety guard that refuses to attach to games protected by anti-cheat systems (`EasyAntiCheat`, `BattlEye`, `Vanguard`, `Ricochet / Randgrid.sys`, `ACE`, etc.). PadSplit **never** attempts to bypass or tamper with any kernel driver or anti-cheat mechanism.

For protected games, use the game's built-in **Borderless Fullscreen** + **Disable Mute on Focus Loss** settings instead.

### Windows Defender / SmartScreen Warning
Because PadSplit injects its own core DLL (`padsplit_core_x64.dll` / `padsplit_core_x86.dll`) into game processes using `CreateRemoteThread` + `LoadLibraryW`, heuristic antivirus engines may occasionally flag unsigned binaries as false positives.
- Every release asset is compiled transparently from this repository and ships with `SHA256SUMS.txt`.
- You can inspect all ~2,500 lines of C++ source and build identical binaries yourself with `python build.py`.

### Elevated (Admin) Games
If a game runs as Administrator, Windows blocks injection from a non-elevated process (`ERROR_ACCESS_DENIED`). Run `PadSplit.exe` (or your terminal for `padsplit_cli.exe`) as Administrator when attaching to elevated games.

### Steam Input Tip
If your Xbox/PlayStation controller switches to "Desktop Configuration" (moving the mouse cursor) once you click on Monitor 2 in a Steam game, right-click the game in Steam -> **Properties -> Controller -> Disable Steam Input**.

---

## CLI Reference (`padsplit_cli.exe`)

```powershell
padsplit_cli.exe list [--games-only] [--json]
padsplit_cli.exe inspect <pid|name> [--json]
padsplit_cli.exe attach <pid|name> [--mode gamepad-only|border-crossing] [--borderless off|window|force-dx] [--idle-hide 0|1] [--idle-ms N]
padsplit_cli.exe status [pid|name] [--json]
padsplit_cli.exe set <pid|name> [--enabled 0|1] [--mode ...] [--borderless ...] [--idle-hide 0|1] [--idle-ms N] [--apply-borderless]
padsplit_cli.exe pause <pid|name>
padsplit_cli.exe resume <pid|name>
padsplit_cli.exe logs <pid|name> [--follow] [--tail N]
padsplit_cli.exe detach <pid|name>
padsplit_cli.exe whitelist list [--json]
padsplit_cli.exe whitelist add <exe> [--mode ...] [--borderless ...]
padsplit_cli.exe whitelist remove <exe>
padsplit_cli.exe ac-check <pid|path> [--json]
padsplit_cli.exe monitors [--json]
padsplit_cli.exe autostart on | off | status
padsplit_cli.exe daemon ping | reload | stop
padsplit_cli.exe --version
```

---

## Project Architecture

| Directory | Purpose |
| :--- | :--- |
| `src/core/` | In-process hook DLL (`padsplit_core_x64.dll` / `padsplit_core_x86.dll`): focus spoofing, cursor unlocking, RawInput/XInput/DInput8/GameInput hooks, borderless window/DXGI/D3D9 hooks, DirectSound global focus |
| `src/injector/` | Same-arch x64 injector library & lightweight x86 helper (`padsplit_helper_x86.exe`) for 32-bit games, plus shared-memory IPC (`SharedState`) |
| `src/app/` | System-tray daemon & DX11 + Dear ImGui bilingual control panel (`PadSplit.exe`), whitelist auto-attach, global hotkeys, Windows resources |
| `src/cli/` | Scriptable command-line controller (`padsplit_cli.exe`) for diagnostics, automation, and testing |
| `src/shared/` | Shared IPC protocol, JSON config persistence, anti-cheat guard, Win32 helpers |
| `src/test/` | Dual-arch (`x64` & `x86`) mock game harness simulating `ClipCursor`, `SetCapture`, `RIDEV_CAPTUREMOUSE`, and `XInput` |

---

## License

Released under the [MIT License](LICENSE). See [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) for third-party dependency attributions.
