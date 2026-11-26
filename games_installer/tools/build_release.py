#!/usr/bin/env python3
"""Package the current Windows WebView2 installer from verified local payloads."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    os.chdir(ROOT)
    subprocess.run([sys.executable, "tools/stage_payload.py", "--check"], check=True)
    os.environ.update(GOOS="windows", GOARCH="amd64", CGO_ENABLED="0")
    dest = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "dist"
    dest.mkdir(parents=True, exist_ok=True)
    # ID6_INSTALLER_NAME and ID6_BUILD_LABEL name the build (e.g. "Game Room v34").
    name = os.environ.get("ID6_INSTALLER_NAME", "ID6 Game Room Installer")
    label = os.environ.get("ID6_BUILD_LABEL", "")
    ldflags = "-s -w -H windowsgui" + (f" -X 'main.buildLabel={label}'" if label else "")
    binary = dest / f"{name}.exe"
    subprocess.run(["go", "build", "-mod=vendor", "-trimpath", "-buildvcs=false", "-ldflags=" + ldflags, "-o", str(binary), "."], check=True)
    archive = dest / (name.replace(" - ", "-").replace(" ", "-") + "-Windows-x64.zip" if label else "ID6-Game-Room-Installer-Windows-x64.zip")
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(binary, binary.name)
        z.write(ROOT / "README.md", "Quick Start.md")
        for file in sorted((ROOT / "third-party").rglob("*")):
            if file.is_file():
                z.write(file, file.relative_to(ROOT))
    files = sorted(p for p in (ROOT / "payload").rglob("*") if p.is_file())
    hashes = {p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
    hashes[binary.name] = hashlib.sha256(binary.read_bytes()).hexdigest()
    hashes[archive.name] = hashlib.sha256(archive.read_bytes()).hexdigest()
    (dest / "SHA256SUMS.json").write_text(json.dumps(hashes, indent=2) + "\n")
    print("Built", binary)
    print("Packaged", archive)


if __name__ == "__main__":
    main()
