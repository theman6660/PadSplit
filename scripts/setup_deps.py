import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLS_DIR = ROOT / "tools"
THIRD_PARTY = ROOT / "third_party"

# Pinned dependency versions for reproducible builds
LLVM_TAG = "20261117"
LLVM_URL = f"https://github.com/mstorsjo/llvm-mingw/releases/download/{LLVM_TAG}/llvm-mingw-{LLVM_TAG}-ucrt-x86_64.zip"
LLVM_DIR = Path(os.environ.get("LLVM_MINGW_DIR", str(TOOLS_DIR / "llvm-mingw")))

MINHOOK_REPO = "https://github.com/TsudaKageyu/minhook.git"
MINHOOK_COMMIT = "8af6b4acae5a9388fd742b56fa79ece89d96f823"

IMGUI_REPO = "https://github.com/ocornut/imgui.git"
IMGUI_COMMIT = "77fe4588e6af934b4167b91312174ca3fbb0d71a"

JSON_VERSION = "v3.12.0"
JSON_URL = f"https://github.com/nlohmann/json/releases/download/{JSON_VERSION}/json.hpp"


def download_file(url: str, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    print(f"[setup] Downloading {url} -> {dest}")
    req = urllib.request.Request(url, headers={"User-Agent": "PadSplitBuilder/1.0"})
    with urllib.request.urlopen(req, timeout=180) as resp, open(dest, "wb") as f:
        total = int(resp.headers.get("Content-Length", "0") or "0")
        downloaded = 0
        while True:
            chunk = resp.read(1024 * 256)
            if not chunk:
                break
            f.write(chunk)
            downloaded += len(chunk)
            if total > 0 and downloaded % (16 * 1024 * 1024) < len(chunk):
                pct = downloaded * 100 // total
                print(f"[setup]   {pct}% ({downloaded // (1024 * 1024)} MB / {total // (1024 * 1024)} MB)")


def setup_llvm_mingw() -> None:
    x64_clang = LLVM_DIR / "bin" / "x86_64-w64-mingw32-clang++.exe"
    x86_clang = LLVM_DIR / "bin" / "i686-w64-mingw32-clang++.exe"
    if x64_clang.exists() and x86_clang.exists():
        print(f"[setup] llvm-mingw already present at {LLVM_DIR}")
        return

    TOOLS_DIR.mkdir(parents=True, exist_ok=True)
    zip_path = TOOLS_DIR / f"llvm-mingw-{LLVM_TAG}.zip"
    if not zip_path.exists():
        download_file(LLVM_URL, zip_path)

    print(f"[setup] Extracting {zip_path} ...")
    extract_tmp = TOOLS_DIR / "_extract_tmp"
    if extract_tmp.exists():
        shutil.rmtree(extract_tmp)
    with zipfile.ZipFile(zip_path, "r") as zf:
        zf.extractall(extract_tmp)

    subdirs = [p for p in extract_tmp.iterdir() if p.is_dir()]
    if len(subdirs) != 1:
        raise RuntimeError(f"Unexpected zip layout in {extract_tmp}: {subdirs}")

    if LLVM_DIR.exists():
        shutil.rmtree(LLVM_DIR)
    LLVM_DIR.parent.mkdir(parents=True, exist_ok=True)
    shutil.move(str(subdirs[0]), str(LLVM_DIR))
    shutil.rmtree(extract_tmp, ignore_errors=True)
    zip_path.unlink(missing_ok=True)

    if not x64_clang.exists() or not x86_clang.exists():
        raise RuntimeError("llvm-mingw extraction missing x64 or x86 clang++!")
    print(f"[setup] llvm-mingw ready at {LLVM_DIR}")


def git_fetch_pinned(repo_url: str, commit: str, dest: Path) -> None:
    if dest.exists() and any(dest.iterdir()):
        print(f"[setup] {dest.name} already present at {dest}")
        return
    dest.mkdir(parents=True, exist_ok=True)
    print(f"[setup] Fetching {repo_url} @ {commit[:12]} -> {dest}")
    subprocess.run(["git", "init", "-q"], cwd=str(dest), check=True)
    subprocess.run(["git", "remote", "add", "origin", repo_url], cwd=str(dest), check=True)
    subprocess.run(["git", "fetch", "--depth", "1", "origin", commit], cwd=str(dest), check=True)
    subprocess.run(["git", "checkout", "-q", "FETCH_HEAD"], cwd=str(dest), check=True)


def setup_json() -> None:
    dest = THIRD_PARTY / "nlohmann" / "json.hpp"
    if dest.exists() and dest.stat().st_size > 10000:
        print(f"[setup] nlohmann/json.hpp already present at {dest}")
        return
    download_file(JSON_URL, dest)


def main() -> int:
    deps_only = "--deps-only" in sys.argv[1:]
    if not deps_only:
        setup_llvm_mingw()
    git_fetch_pinned(MINHOOK_REPO, MINHOOK_COMMIT, THIRD_PARTY / "minhook")
    git_fetch_pinned(IMGUI_REPO, IMGUI_COMMIT, THIRD_PARTY / "imgui")
    setup_json()
    print("[setup] All dependencies ready.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
