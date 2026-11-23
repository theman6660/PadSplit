"""Package dist/ binaries and docs into a clean portable Release zip with SHA256 checksums."""

import hashlib
import re
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIST = ROOT / "dist"
OUT_DIR = ROOT / "build" / "release"

BINARIES = [
    "PadSplit.exe",
    "padsplit_cli.exe",
    "padsplit_core_x64.dll",
    "padsplit_core_x86.dll",
    "padsplit_helper_x86.exe",
]

DOCS = [
    "README.md",
    "README_CN.md",
    "LICENSE",
    "THIRD_PARTY_LICENSES.md",
]


def read_version() -> str:
    vh = (ROOT / "src" / "shared" / "version.h").read_text(encoding="utf-8")
    m = re.search(r'#define\s+PADSPLIT_VERSION_STR\s+"([^"]+)"', vh)
    if not m:
        raise RuntimeError("Could not parse PADSPLIT_VERSION_STR from src/shared/version.h")
    return m.group(1)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    version = read_version()
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    zip_name = f"PadSplit-v{version}-win-x64.zip"
    zip_path = OUT_DIR / zip_name

    for b in BINARIES:
        p = DIST / b
        if not p.exists():
            print(f"[package] Missing binary: {p}", file=sys.stderr)
            return 1

    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for b in BINARIES:
            zf.write(DIST / b, arcname=f"PadSplit/{b}")
        for d in DOCS:
            p = ROOT / d
            if p.exists():
                zf.write(p, arcname=f"PadSplit/{d}")

    checksums = []
    for b in BINARIES:
        checksums.append(f"{sha256_file(DIST / b)}  {b}")
    checksums.append(f"{sha256_file(zip_path)}  {zip_name}")

    sums_path = OUT_DIR / "SHA256SUMS.txt"
    sums_path.write_text("\n".join(checksums) + "\n", encoding="utf-8")

    print(f"[package] Created {zip_path} ({zip_path.stat().st_size:,} bytes)")
    print(f"[package] Created {sums_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
