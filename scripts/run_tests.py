import json
import os
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIST = ROOT / "dist"
TEST_BIN = ROOT / "build" / "test"
CLI = DIST / "padsplit_cli.exe"
LLVM_DIR = Path(os.environ.get("LLVM_MINGW_DIR", str(ROOT / "tools" / "llvm-mingw")))
OBJDUMP = LLVM_DIR / "bin" / "llvm-objdump.exe"
if not OBJDUMP.exists():
    found_od = shutil.which("llvm-objdump.exe") or shutil.which("llvm-objdump")
    if found_od:
        OBJDUMP = Path(found_od)


def run_cli(args: list[str], check: bool = True) -> subprocess.CompletedProcess:
    cmd = [str(CLI)] + args
    res = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if check and res.returncode != 0:
        raise AssertionError(f"CLI failed ({res.returncode}): {' '.join(cmd)}\nstdout={res.stdout}\nstderr={res.stderr}")
    return res


def test_dll_dependencies() -> None:
    print("[test] 1. Auditing core DLL imports (zero runtime deps)...")
    forbidden = [
        "libstdc++", "libc++", "libgcc", "libwinpthread",
        "dinput8", "dxgi", "d3d9", "dsound", "xinput", "gameinput"
    ]
    for dll_name in ("padsplit_core_x64.dll", "padsplit_core_x86.dll"):
        dll_path = DIST / dll_name
        assert dll_path.exists(), f"Missing {dll_path}"
        res = subprocess.run([str(OBJDUMP), "-p", str(dll_path)], capture_output=True, text=True, check=True)
        dll_lines = [line.strip() for line in res.stdout.splitlines() if "DLL Name:" in line]
        print(f"  {dll_name} imports: {dll_lines}")
        for line in dll_lines:
            low = line.lower()
            for bad in forbidden:
                assert bad not in low, f"Forbidden dependency '{bad}' in {dll_name}: {line}"


def test_monitors() -> None:
    print("[test] 2. Checking monitor enumeration...")
    res = run_cli(["monitors", "--json"])
    mons = json.loads(res.stdout)
    assert isinstance(mons, list) and len(mons) >= 1, f"Unexpected monitors output: {res.stdout}"
    for m in mons:
        assert isinstance(m["left"], int) and isinstance(m["top"], int)
    print(f"  Detected {len(mons)} monitor(s): {[(m['index'], m['left'], m['top'], m['width'], m['height'], m['primary']) for m in mons]}")


def test_whitelist_roundtrip() -> None:
    print("[test] 3. Testing whitelist add / list / remove round-trip...")
    cfg_path = DIST / "config.json"
    existed_before = cfg_path.exists()
    backup = cfg_path.read_bytes() if existed_before else None
    try:
        test_exe = "__padsplit_unit_test__.exe"
        run_cli(["whitelist", "add", test_exe, "--mode", "border-crossing", "--borderless", "force-dx"])
        wl = json.loads(run_cli(["whitelist", "list", "--json"]).stdout)
        found = [item for item in wl if item["exe_name"] == test_exe]
        assert len(found) == 1, f"Whitelist entry not found: {wl}"
        assert found[0]["isolation_mode"] == "border-crossing"
        assert found[0]["borderless_mode"] == "force-dx"
        run_cli(["whitelist", "remove", test_exe])
        wl2 = json.loads(run_cli(["whitelist", "list", "--json"]).stdout)
        assert not any(item["exe_name"] == test_exe for item in wl2), "Whitelist remove failed"
        print("  PASS whitelist round-trip")
    finally:
        if existed_before and backup is not None:
            cfg_path.write_bytes(backup)
        elif not existed_before and cfg_path.exists():
            cfg_path.unlink()


def test_anticheat_check() -> None:
    print("[test] 4. Testing anti-cheat detector...")
    safe_res = json.loads(run_cli(["ac-check", str(TEST_BIN / "padsplit_test_game_x64.exe"), "--json"]).stdout)
    assert safe_res["blocked"] is False and safe_res["level"] == "none", f"Expected clear for mock game: {safe_res}"

    ac_res = json.loads(run_cli(["ac-check", "C:\\Games\\VALORANT-Win64-Shipping.exe", "--json"]).stdout)
    assert ac_res["blocked"] is True and ac_res["level"] == "block", f"Expected block for Valorant: {ac_res}"

    cod_res = json.loads(run_cli(["ac-check", "D:\\Games\\sp26-cod.exe", "--json"]).stdout)
    assert cod_res["blocked"] is True and cod_res["level"] == "block", f"Expected block for sp26-cod.exe: {cod_res}"
    print("  PASS anti-cheat detector")


def run_mock_e2e(exe_path: Path, expected_arch: int) -> None:
    print(f"[test] 5. Running E2E lifecycle against {exe_path.name} ({expected_arch}-bit)...")
    assert exe_path.exists(), f"Missing {exe_path}"
    proc = subprocess.Popen(
        [str(exe_path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )

    latest_hb = {}
    hb_lock = threading.Lock()
    stop_reader = threading.Event()

    def reader() -> None:
        assert proc.stdout is not None
        while not stop_reader.is_set():
            line = proc.stdout.readline()
            if not line:
                break
            line = line.strip()
            if line.startswith("{"):
                try:
                    data = json.loads(line)
                    with hb_lock:
                        latest_hb.clear()
                        latest_hb.update(data)
                except json.JSONDecodeError:
                    pass

    t = threading.Thread(target=reader, daemon=True)
    t.start()

    try:
        deadline = time.time() + 5.0
        while time.time() < deadline:
            with hb_lock:
                if "pid" in latest_hb:
                    break
            time.sleep(0.05)
        with hb_lock:
            assert "pid" in latest_hb, "Mock game did not emit heartbeat"
        pid = proc.pid

        ins = json.loads(run_cli(["inspect", str(pid), "--json"]).stdout)
        assert ins["arch"] == expected_arch, f"Expected arch {expected_arch}, got {ins['arch']}"
        assert ins["has_window"] is True, f"Expected has_window=True, got {ins}"
        assert ins["anticheat"]["blocked"] is False, f"Unexpected AC block: {ins['anticheat']}"

        run_cli(["attach", str(pid), "--mode", "gamepad-only", "--borderless", "window"])
        time.sleep(1.0)

        # Regression test: status (no PID arg) and list while attached (uses FILE_MAP_READ)
        all_st = json.loads(run_cli(["status", "--json"]).stdout)
        assert any(item["pid"] == pid for item in all_st), f"Attached PID {pid} missing from status --json: {all_st}"
        run_cli(["list", "--json"])

        st = json.loads(run_cli(["status", str(pid), "--json"]).stdout)
        assert st["core_ready"] == 1, f"core_ready != 1: {st}"
        assert st["core_arch"] == expected_arch, f"core_arch mismatch: {st}"
        assert st["kbm_currently_blocked"] is True, f"Expected kbm_currently_blocked=True: {st}"
        assert "gameinput" in st["detected"], f"Missing detected.gameinput field: {st}"
        c = st["counters"]
        assert c["blocked_clip_cursor"] > 0, f"blocked_clip_cursor == 0: {c}"
        assert c["blocked_set_cursor_pos"] > 0, f"blocked_set_cursor_pos == 0: {c}"
        assert c["blocked_kbm_polls"] > 0, f"blocked_kbm_polls == 0: {c}"
        assert c["rawinput_pad_inputsink_added"] >= 1, f"rawinput_pad_inputsink_added < 1: {c}"
        assert c["xinput_polls"] > 0, f"xinput_polls == 0: {c}"
        assert c["focus_api_spoofs"] > 0, f"focus_api_spoofs == 0: {c}"

        with hb_lock:
            hb_copy = dict(latest_hb)
        assert hb_copy.get("fg_is_self") is True, f"Mock game should see fg_is_self=True when spoofed, got: {hb_copy}"
        assert (hb_copy.get("mouse_raw_flags", 0x230) & 0x230) == 0, f"RIDEV_CAPTUREMOUSE|RIDEV_NOLEGACY not stripped! {hb_copy}"
        assert hb_copy.get("has_capture") is False, f"SetCapture was not released/blocked! {hb_copy}"

        # Live mode switch test
        run_cli(["set", str(pid), "--mode", "border-crossing"])
        st_bc = json.loads(run_cli(["status", str(pid), "--json"]).stdout)
        assert st_bc["isolation_mode"] == "border-crossing", f"Expected border-crossing: {st_bc}"
        run_cli(["set", str(pid), "--mode", "gamepad-only"])

        # Pause & Resume (also verifies original mouse_raw_flags restore & re-strip!)
        run_cli(["pause", str(pid)])
        time.sleep(0.4)
        st_paused = json.loads(run_cli(["status", str(pid), "--json"]).stdout)
        assert st_paused["kbm_currently_blocked"] is False, f"Expected kbm_currently_blocked=False after pause: {st_paused}"
        with hb_lock:
            hb_paused = dict(latest_hb)
        assert (hb_paused.get("mouse_raw_flags", 0) & 0x230) == 0x230, f"Original mouse_raw_flags not restored on pause: {hb_paused}"

        run_cli(["resume", str(pid)])
        time.sleep(0.4)
        st_resumed = json.loads(run_cli(["status", str(pid), "--json"]).stdout)
        assert st_resumed["kbm_currently_blocked"] is True, f"Expected kbm_currently_blocked=True after resume: {st_resumed}"
        with hb_lock:
            hb_resumed = dict(latest_hb)
        assert (hb_resumed.get("mouse_raw_flags", 0x230) & 0x230) == 0, f"mouse_raw_flags not re-stripped on resume: {hb_resumed}"

        # Logs
        logs_out = run_cli(["logs", str(pid)]).stdout
        assert "PadSplit Core ready" in logs_out, f"Missing ready message in logs:\n{logs_out}"

        # Detach
        run_cli(["detach", str(pid)])
        time.sleep(0.5)
        assert proc.poll() is None, f"Mock game crashed or exited on detach! rc={proc.returncode}"
        print(f"  PASS {exe_path.name} ({expected_arch}-bit): spoofs={c['focus_api_spoofs']} clip_blk={c['blocked_clip_cursor']} xinput={c['xinput_polls']}")
    finally:
        stop_reader.set()
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()


def test_gui_and_daemon_smoke() -> None:
    print("[test] 6. Smoke-testing PadSplit.exe --show (full DX11 + ImGui panel) + daemon ping/reload/stop...")
    app_exe = DIST / "PadSplit.exe"
    assert app_exe.exists(), f"Missing {app_exe}"
    proc = subprocess.Popen([str(app_exe), "--show"])
    try:
        time.sleep(1.2)
        assert proc.poll() is None, f"PadSplit.exe exited prematurely with rc={proc.returncode}"
        ping_res = run_cli(["daemon", "ping"])
        assert "pong" in ping_res.stdout.lower(), f"Expected pong, got: {ping_res.stdout}"
        reload_res = run_cli(["daemon", "reload"])
        assert "reloaded" in reload_res.stdout.lower(), f"Expected reloaded, got: {reload_res.stdout}"
        stop_res = run_cli(["daemon", "stop"])
        assert "stopping" in stop_res.stdout.lower(), f"Expected stopping, got: {stop_res.stdout}"
        rc = proc.wait(timeout=4)
        assert rc == 0, f"PadSplit.exe exited with code {rc}"
        print("  PASS PadSplit.exe GUI + daemon ping/reload/stop smoke test")
    finally:
        if proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=3)


def test_cli_version_and_resources() -> None:
    print("[test] 0. Checking CLI --version and PadSplit.exe embedded resources...")
    ver = run_cli(["--version"]).stdout.strip()
    assert ver.startswith("PadSplit CLI v"), f"Unexpected --version output: {ver}"
    app_exe = DIST / "PadSplit.exe"
    assert app_exe.exists(), f"Missing {app_exe}"
    res = subprocess.run([str(OBJDUMP), "-h", str(app_exe)], capture_output=True, text=True, check=True)
    assert ".rsrc" in res.stdout, f"PadSplit.exe missing embedded .rsrc section:\n{res.stdout}"
    print(f"  PASS {ver} + embedded .rsrc verified")


def main() -> int:
    test_cli_version_and_resources()
    test_dll_dependencies()
    test_monitors()
    test_whitelist_roundtrip()
    test_anticheat_check()
    run_mock_e2e(TEST_BIN / "padsplit_test_game_x64.exe", 64)
    run_mock_e2e(TEST_BIN / "padsplit_test_game_x86.exe", 32)
    test_gui_and_daemon_smoke()
    print("\nALL TESTS PASSED!")
    return 0


if __name__ == "__main__":
    sys.exit(main())
