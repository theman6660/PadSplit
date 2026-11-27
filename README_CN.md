# PadSplit — 双屏手柄分身工具

一边用**手柄在主屏全屏玩单机游戏**，一边用**鼠标和键盘在副屏自由操作任何原生软件（浏览器、IDE、AI Agent、聊天软件等）**——主屏游戏不最小化、不断声音、不暂停、不被副屏点击或打字串扰。

[![Build & Test](https://github.com/theman6660/PadSplit/actions/workflows/build.yml/badge.svg)](https://github.com/theman6660/PadSplit/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Windows%20x64%20%2F%20x86-0078D4.svg)](#)

[English](README.md) | **简体中文**

---

## 解决什么痛点？

Windows 同一时刻只允许一个“前台焦点窗口”。双屏环境下，当你把鼠标切去副屏操作其他软件时，绝大多数游戏都会出现：
1. 鼠标被锁死在主屏边界、强行拉回中心，或导致副屏无法点击（`ClipCursor`、`SetCursorPos`、`SetCapture`、`RIDEV_CAPTUREMOUSE`）；
2. 游戏检测到失焦后立刻暂停、静音、降帧或停止响应手柄（`WM_KILLFOCUS`、`WM_ACTIVATEAPP`、`GetForegroundWindow`）；
3. 独占全屏游戏被直接最小化弹回桌面。

**PadSplit** 通过轻量级用户态 Hook 在单机游戏进程内解决焦点与输入隔离问题——无需虚拟机、无需魔改副屏软件，副屏的输入法、IDE、浏览器和 Agent 保持 100% 原生体验。

```text
+-----------------------------------+     +-----------------------------------+
|  主屏 (无边框全屏单机游戏)          |     |  副屏 (任意原生 Windows 软件)      |
|                                   |     |                                   |
|  [游戏进程 + padsplit_core]        |     |  浏览器 / IDE / AI Agent / 聊天   |
|   * 焦点欺骗：始终保持前台状态      |     |   * 完整鼠标点击与键盘输入焦点     |
|   * 手柄后台持续满血响应           |     |   * 原生中文输入法与拖拽           |
|   * 彻底屏蔽副屏键鼠串扰           |     |   * 鼠标无阻力跨屏、绝不拉回       |
|   * 画面、声音与帧率不中断         |     |                                   |
+-----------------------------------+     +-----------------------------------+
               ^                                           ^
            [手柄]                                     [键盘 & 鼠标]
```

## 核心特性

- **原生 x64 + x86 双架构支持**：内置 64 位与 32 位核心 DLL（`padsplit_core_x64.dll` / `padsplit_core_x86.dll`）及 32 位注入助手，**零外部运行时 DLL 依赖**。
- **焦点欺骗**：拦截 `WM_KILLFOCUS` / `WM_ACTIVATE` / `WM_ACTIVATEAPP` / `WM_NCACTIVATE`、`PeekMessage` 与 `GetForegroundWindow` / `GetActiveWindow` / `GetFocus`，兼容 Unicode 与老 32 位 ANSI 游戏窗口。
- **鼠标解放与双模式防串扰**：
  - 释放 `ClipCursor`、拦截 `SetCursorPos` / `SetCapture`，并剥离 Raw Input 鼠标/键盘的 `RIDEV_CAPTUREMOUSE`、`RIDEV_NOLEGACY`、`RIDEV_NOHOTKEYS` 标志，确保副屏可正常点击和打字。
  - **手柄独占隔离（默认）**：接管后游戏彻底屏蔽键鼠输入、只认手柄，鼠标无阻力滑向副屏且绝不甩镜头（推荐 FPS / 3D 动作游戏）。
  - **越界隔离（可选）**：鼠标在主屏可正常操作游戏，滑出到副屏时自动隔离键鼠（适合 2D / 策略游戏）。
  - **静止自动隐藏光标**：鼠标停留在主屏超过设定时间（默认 `1500 ms`）自动隐藏，移动立即恢复。
- **全通道手柄后台保活**：
  - **XInput**（`xinput1_4` / `xinput1_3` / `xinput9_1_0`，拦截 `XInputEnable(FALSE)`）
  - **DirectInput 8**（自动改写为 `DISCL_BACKGROUND | DISCL_NONEXCLUSIVE` 并自动重获设备）
  - **Raw Input**（自动为手柄/摇杆/多轴 HID 设备补齐 `RIDEV_INPUTSINK`，并将后台 HID 消息的 `RIM_INPUTSINK` 改写为 `RIM_INPUT`）
  - **Windows.Gaming.Input (WinRT)**（Hook `IGamepad::GetCurrentReading` 与 `IRawGameController::GetCurrentReading`，游戏在后台时无缝桥接实时 XInput 状态——完美解决 Unity 2021+/Unity 6 及 WGI 游戏切屏失灵问题）
  - **Microsoft GameInput (v1–v4)**（自动开启后台输入策略并过滤键鼠读取）
  - **DirectSound**（自动附加 `DSBCAPS_GLOBALFOCUS` 兜底老游戏后台静音问题）
- **分层全屏保持**：默认采用零渲染侵入的窗口去边框铺满主屏（兼容 ReShade / RTSS / DLSS 覆盖层）；对仅支持独占全屏的老游戏提供可选 `force-dx`（DXGI / D3D9 强转无边框）。
- **内置反作弊安全拦截**：注入前自动检查进程名、进程已加载模块、运行中的反作弊服务/内核驱动（`Randgrid.sys`、`EasyAntiCheat`、`BEDaisy`、`vgk.sys` 等）及游戏目录，命中即拒绝注入。
- **中英双语托盘界面 + 完整 CLI**：默认静默驻留系统托盘（关面板时 0% GPU 占用），界面自动跟随系统语言并支持中/英一键切换，附带完整命令行工具 `padsplit_cli.exe`（支持 JSON 状态查询与实时日志流）。

---

## 快速上手

### 方式一：下载便携版 Release
1. 前往 [Releases](https://github.com/theman6660/PadSplit/releases) 页面下载最新的 `PadSplit-v*-win-x64.zip`，解压到任意目录即可使用（纯便携，无需安装）。
2. 双击运行 `PadSplit.exe`：
   - 默认静默缩在右下角系统托盘（双击托盘图标，或再次运行 `PadSplit.exe` 可唤出控制面板）。
   - 可在「全局设置」中勾选 **“开机自动静默启动到系统托盘”**。
3. 进入任意离线单机游戏后按一次 **`Ctrl + Alt + P`** 即可接管并自动记入白名单（以后启动自动接管）。
4. 随时按 **`Ctrl + Alt + G`** 临时暂停/恢复键鼠隔离（把鼠标临时还给游戏点菜单）。

### 方式二：从源码编译与测试

需要 Python 3.10+、Git 以及 [Ninja](https://ninja-build.org/)（`pip install ninja`）。

```powershell
git clone https://github.com/theman6660/PadSplit.git
cd PadSplit

# 自动下载固定版本的便携 llvm-mingw、MinHook、Dear ImGui、nlohmann/json
python scripts/setup_deps.py

# 编译全部 x64 / x86 产物至 dist/
python build.py

# 运行 DLL 依赖审计、反作弊测试、x64/x86 模拟游戏端到端测试与 GUI 冒烟测试
python scripts/run_tests.py

# （可选）打包便携 Release 压缩包及 SHA256SUMS.txt 至 build/release/
python scripts/package_release.py
```

---

## 使用须知与常见问题

### 仅限离线单机游戏（反作弊声明）
PadSplit 严格定位于**离线单机游戏**，采用标准用户态 DLL 注入与 API Hook（MinHook），内置反作弊安全拦截，遇到加载反作弊模块或内核保护驱动（如 `EasyAntiCheat`、`BattlEye`、`Vanguard`、`Randgrid.sys`、`ACE` 等）的游戏会**主动拒绝注入，绝不尝试绕过任何保护机制**。

对于带内核保护的游戏，建议直接使用游戏内自带的 **无边框全屏 + 关闭后台静音** 选项。

### 关于杀毒软件 / Windows Defender 误报
由于核心原理需要向目标游戏进程注入 `padsplit_core_x64.dll` / `padsplit_core_x86.dll`（`CreateRemoteThread` + `LoadLibraryW`），未签名二进制文件可能会触发部分杀软的启发式拦截。
- 所有发布包均附带 `SHA256SUMS.txt`，并由公开的 GitHub Actions 流水线透明构建；
- 全部核心代码仅约 2500 行 C++，欢迎直接审查源码并在本地运行 `python build.py` 自行编译。

### 管理员权限提示
如果某款游戏是以“管理员身份”运行的，普通权限下的 PadSplit 会因 Windows UIPI/权限边界被拒绝（`ERROR_ACCESS_DENIED`），此时右键“以管理员身份运行” `PadSplit.exe` 即可。

### Steam 游戏小贴士
如果鼠标切去副屏后 Xbox/PS 手柄突然变成“桌面光标模式”，请在 Steam 库中右键该游戏 → **属性 → 控制器 → 禁用 Steam 输入**。

---

## 命令行工具 (`padsplit_cli.exe`)

```powershell
padsplit_cli.exe list [--games-only] [--json]
padsplit_cli.exe inspect <pid|exe> [--json]
padsplit_cli.exe attach <pid|exe> [--mode gamepad-only|border-crossing] [--borderless off|window|force-dx] [--idle-hide 0|1] [--idle-ms N]
padsplit_cli.exe status [pid|exe] [--json]
padsplit_cli.exe set <pid|exe> [--enabled 0|1] [--mode ...] [--borderless ...] [--idle-hide 0|1] [--idle-ms N] [--apply-borderless]
padsplit_cli.exe pause <pid|exe>
padsplit_cli.exe resume <pid|exe>
padsplit_cli.exe logs <pid|exe> [--follow] [--tail N]
padsplit_cli.exe detach <pid|exe>
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

## 开源协议

基于 [MIT License](LICENSE) 开源，第三方依赖协议详见 [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)。
