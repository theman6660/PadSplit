import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
LLVM_DIR = Path(os.environ.get("LLVM_MINGW_DIR", str(ROOT / "tools" / "llvm-mingw")))
LLVM_BIN = LLVM_DIR / "bin"
MINHOOK = ROOT / "third_party" / "minhook"
IMGUI = ROOT / "third_party" / "imgui"
NLOHMANN_INC = ROOT / "third_party"

BUILD_DIR = ROOT / "build"
OBJ_DIR = BUILD_DIR / "obj"
TEST_DIR = BUILD_DIR / "test"
DIST_DIR = ROOT / "dist"


def npath(p: Path) -> str:
    return p.as_posix().replace(":", "$:")


def find_tool(name: str) -> Path | None:
    candidate = LLVM_BIN / name
    if candidate.exists():
        return candidate
    found = shutil.which(name)
    return Path(found) if found else None


def main() -> int:
    debug_mode = "--debug" in sys.argv[1:]
    ninja_args = [a for a in sys.argv[1:] if a != "--debug"]

    x64_cxx = find_tool("x86_64-w64-mingw32-clang++.exe")
    x64_cc = find_tool("x86_64-w64-mingw32-clang.exe")
    x86_cxx = find_tool("i686-w64-mingw32-clang++.exe")
    x86_cc = find_tool("i686-w64-mingw32-clang.exe")
    x64_windres = find_tool("x86_64-w64-mingw32-windres.exe")

    for label, tool in [
        ("x86_64-w64-mingw32-clang++", x64_cxx),
        ("x86_64-w64-mingw32-clang", x64_cc),
        ("i686-w64-mingw32-clang++", x86_cxx),
        ("i686-w64-mingw32-clang", x86_cc),
        ("x86_64-w64-mingw32-windres", x64_windres),
    ]:
        if not tool:
            print(f"[build] Missing compiler/tool: {label}. Run `python scripts/setup_deps.py` first.", file=sys.stderr)
            return 1

    if not shutil.which("ninja"):
        print("[build] Missing `ninja` in PATH. Install via `pip install ninja` or winget.", file=sys.stderr)
        return 1

    for dep in (MINHOOK / "include" / "MinHook.h", IMGUI / "imgui.h", NLOHMANN_INC / "nlohmann" / "json.hpp"):
        if not dep.exists():
            print(f"[build] Missing dependency: {dep}. Run `python scripts/setup_deps.py` first.", file=sys.stderr)
            return 1

    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    OBJ_DIR.mkdir(parents=True, exist_ok=True)
    TEST_DIR.mkdir(parents=True, exist_ok=True)
    DIST_DIR.mkdir(parents=True, exist_ok=True)

    opt_flag = "-O0 -g" if debug_mode else "-O2"
    strip_flag = "" if debug_mode else "-s"

    lines = [
        f"cxx64 = {npath(x64_cxx)}",
        f"cc64  = {npath(x64_cc)}",
        f"cxx32 = {npath(x86_cxx)}",
        f"cc32  = {npath(x86_cc)}",
        f"rc64  = {npath(x64_windres)}",
        "",
        "rule cxx64",
        "  depfile = $out.d",
        "  deps = gcc",
        f"  command = $cxx64 -std=c++20 {opt_flag} -Wall -Wextra -Wno-unused-parameter -DUNICODE -D_UNICODE -MMD -MF $out.d $includes -c $in -o $out",
        "  description = CXX64 $in",
        "",
        "rule cc64",
        "  depfile = $out.d",
        "  deps = gcc",
        f"  command = $cc64 {opt_flag} -DUNICODE -D_UNICODE -MMD -MF $out.d $includes -c $in -o $out",
        "  description = CC64 $in",
        "",
        "rule cxx32",
        "  depfile = $out.d",
        "  deps = gcc",
        f"  command = $cxx32 -std=c++20 {opt_flag} -Wall -Wextra -Wno-unused-parameter -DUNICODE -D_UNICODE -MMD -MF $out.d $includes -c $in -o $out",
        "  description = CXX32 $in",
        "",
        "rule cc32",
        "  depfile = $out.d",
        "  deps = gcc",
        f"  command = $cc32 {opt_flag} -DUNICODE -D_UNICODE -MMD -MF $out.d $includes -c $in -o $out",
        "  description = CC32 $in",
        "",
        "rule rc64",
        f"  command = $rc64 -I {npath(ROOT)} -I {npath(ROOT / 'src')} -O coff $in -o $out",
        "  description = RC64 $in",
        "",
        "rule link64",
        f"  command = $cxx64 {opt_flag} {strip_flag} -static -static-libgcc -static-libstdc++ $ldflags $in -o $out $libs",
        "  description = LINK64 $out",
        "",
        "rule link32",
        f"  command = $cxx32 {opt_flag} {strip_flag} -static -static-libgcc -static-libstdc++ $ldflags $in -o $out $libs",
        "  description = LINK32 $out",
        "",
    ]

    def add_cc(rule: str, arch_tag: str, src: Path, includes: str) -> str:
        rel = src.relative_to(ROOT).as_posix().replace("/", "__")
        obj = OBJ_DIR / f"{arch_tag}__{rel}.o"
        lines.append(f"build {npath(obj)}: {rule} {npath(src)}")
        lines.append(f"  includes = {includes}")
        return npath(obj)

    inc_shared = f"-I{npath(ROOT / 'src')} -I{npath(NLOHMANN_INC)}"
    inc_core = f"-I{npath(ROOT / 'src')} -I{npath(MINHOOK / 'include')}"
    inc_imgui = f"-I{npath(ROOT / 'src')} -I{npath(NLOHMANN_INC)} -I{npath(IMGUI)} -I{npath(IMGUI / 'backends')}"

    # 1. padsplit_core_x64.dll
    core_cpp = [
        ROOT / "src/core/state.cpp",
        ROOT / "src/core/hooks_focus.cpp",
        ROOT / "src/core/hooks_cursor.cpp",
        ROOT / "src/core/hooks_input.cpp",
        ROOT / "src/core/hooks_display.cpp",
        ROOT / "src/core/hooks_audio.cpp",
        ROOT / "src/core/dllmain.cpp",
    ]
    mh_common_c = [
        MINHOOK / "src/buffer.c",
        MINHOOK / "src/hook.c",
        MINHOOK / "src/trampoline.c",
    ]

    core64_objs = [add_cc("cxx64", "core64", p, inc_core) for p in core_cpp]
    for p in mh_common_c + [MINHOOK / "src/hde/hde64.c"]:
        core64_objs.append(add_cc("cc64", "core64", p, inc_core))

    core64_out = npath(DIST_DIR / "padsplit_core_x64.dll")
    lines.append(f"build {core64_out}: link64 {' '.join(core64_objs)}")
    lines.append("  ldflags = -shared")
    lines.append("  libs = -luser32 -lgdi32 -lkernel32")
    lines.append("")

    # 2. padsplit_core_x86.dll
    core32_objs = [add_cc("cxx32", "core32", p, inc_core) for p in core_cpp]
    for p in mh_common_c + [MINHOOK / "src/hde/hde32.c"]:
        core32_objs.append(add_cc("cc32", "core32", p, inc_core))

    core32_out = npath(DIST_DIR / "padsplit_core_x86.dll")
    lines.append(f"build {core32_out}: link32 {' '.join(core32_objs)}")
    lines.append("  ldflags = -shared")
    lines.append("  libs = -luser32 -lgdi32 -lkernel32")
    lines.append("")

    # 3. padsplit_helper_x86.exe
    helper_obj = add_cc("cxx32", "helper32", ROOT / "src/injector/helper_x86.cpp", inc_shared)
    helper_out = npath(DIST_DIR / "padsplit_helper_x86.exe")
    lines.append(f"build {helper_out}: link32 {helper_obj}")
    lines.append("  ldflags = -mwindows -municode")
    lines.append("  libs = -lshell32 -luser32 -lkernel32")
    lines.append("")

    # Shared + Injector objects (x64)
    shared_inj_cpp = [
        ROOT / "src/shared/win_util.cpp",
        ROOT / "src/shared/anticheat.cpp",
        ROOT / "src/shared/config.cpp",
        ROOT / "src/injector/injector_lib.cpp",
    ]
    shared64_objs = [add_cc("cxx64", "shared64", p, inc_shared) for p in shared_inj_cpp]

    # 4. padsplit_cli.exe
    cli_obj = add_cc("cxx64", "cli64", ROOT / "src/cli/cli_main.cpp", inc_shared)
    cli_out = npath(DIST_DIR / "padsplit_cli.exe")
    lines.append(f"build {cli_out}: link64 {cli_obj} {' '.join(shared64_objs)}")
    lines.append("  ldflags =")
    lines.append("  libs = -luser32 -lkernel32 -ladvapi32")
    lines.append("")

    # 5. PadSplit.exe (with embedded icon, manifest, and version info)
    imgui_cpp = [
        IMGUI / "imgui.cpp",
        IMGUI / "imgui_draw.cpp",
        IMGUI / "imgui_tables.cpp",
        IMGUI / "imgui_widgets.cpp",
        IMGUI / "backends/imgui_impl_win32.cpp",
        IMGUI / "backends/imgui_impl_dx11.cpp",
    ]
    app_cpp = [
        ROOT / "src/app/controller.cpp",
        ROOT / "src/app/ui_panel.cpp",
        ROOT / "src/app/app_main.cpp",
    ]
    app_objs = [add_cc("cxx64", "app64", p, inc_imgui) for p in app_cpp]
    imgui_objs = [add_cc("cxx64", "imgui64", p, inc_imgui) for p in imgui_cpp]

    res_obj = npath(OBJ_DIR / "app64__padsplit.res.o")
    res_rc = npath(ROOT / "src/app/padsplit.rc")
    res_ico = npath(ROOT / "assets/padsplit.ico")
    res_manifest = npath(ROOT / "src/app/padsplit.manifest")
    lines.append(f"build {res_obj}: rc64 {res_rc} | {res_ico} {res_manifest}")
    lines.append("")

    app_out = npath(DIST_DIR / "PadSplit.exe")
    lines.append(f"build {app_out}: link64 {' '.join(app_objs)} {' '.join(imgui_objs)} {' '.join(shared64_objs)} {res_obj}")
    lines.append("  ldflags = -mwindows -municode")
    lines.append("  libs = -ld3d11 -ldxgi -ld3dcompiler -ldwmapi -lshell32 -luser32 -lgdi32 -lole32 -ladvapi32")
    lines.append("")

    # 6. Test mock games (x64 + x86)
    test64_obj = add_cc("cxx64", "test64", ROOT / "src/test/mock_game.cpp", inc_shared)
    test64_out = npath(TEST_DIR / "padsplit_test_game_x64.exe")
    lines.append(f"build {test64_out}: link64 {test64_obj}")
    lines.append("  ldflags =")
    lines.append("  libs = -luser32 -lgdi32 -lkernel32")
    lines.append("")

    test32_obj = add_cc("cxx32", "test32", ROOT / "src/test/mock_game.cpp", inc_shared)
    test32_out = npath(TEST_DIR / "padsplit_test_game_x86.exe")
    lines.append(f"build {test32_out}: link32 {test32_obj}")
    lines.append("  ldflags =")
    lines.append("  libs = -luser32 -lgdi32 -lkernel32")
    lines.append("")

    ninja_file = BUILD_DIR / "build.ninja"
    ninja_file.write_text("\n".join(lines), encoding="utf-8")
    print(f"[build] Generated {ninja_file}")

    ninja_cmd = ["ninja", "-f", str(ninja_file)] + ninja_args
    res = subprocess.run(ninja_cmd, cwd=str(ROOT))
    if res.returncode != 0:
        return res.returncode

    print("[build] Built artifacts:")
    for f in [
        DIST_DIR / "padsplit_core_x64.dll",
        DIST_DIR / "padsplit_core_x86.dll",
        DIST_DIR / "padsplit_helper_x86.exe",
        DIST_DIR / "padsplit_cli.exe",
        DIST_DIR / "PadSplit.exe",
        TEST_DIR / "padsplit_test_game_x64.exe",
        TEST_DIR / "padsplit_test_game_x86.exe",
    ]:
        if f.exists():
            print(f"  {f.relative_to(ROOT)} : {f.stat().st_size:,} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
