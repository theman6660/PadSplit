---
name: Bug Report / Game Compatibility
about: Report a bug or a single-player game compatibility issue
title: "[Bug]: "
labels: bug
---

### Game & Environment
- **Game name & executable**: (e.g., `DOOMTheDarkAges.exe`)
- **Architecture**: x64 / x86
- **Store / Launcher**: Steam / GOG / Epic / Standalone
- **Windows version**: (e.g., Windows 11 24H2)
- **Monitor layout**: (e.g., 2x 2560x1600, Monitor 1 primary)
- **PadSplit version**: (run `padsplit_cli.exe --version`)

### What happened?
Describe what went wrong (e.g., cursor still locked, clicks blocked on Monitor 2, gamepad stops responding, game pauses/mutes, crash on attach/detach).

### Diagnostic Output
Please attach the output of these two commands while the game is running & attached:

```powershell
padsplit_cli.exe status <exe_name> --json
padsplit_cli.exe logs <exe_name>
```

> **Note**: PadSplit intentionally refuses to attach to games protected by anti-cheat systems (`EasyAntiCheat`, `BattlEye`, `Vanguard`, `Randgrid.sys`, etc.). Please do not file requests to bypass anti-cheat.
