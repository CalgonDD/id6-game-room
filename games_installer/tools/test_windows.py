#!/usr/bin/env python3
"""Run the host suites with explicit Cygwin and MinGW tool selections."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--posix-bin", type=Path, default=os.environ.get("ID6_POSIX_BIN"), help="Cygwin bin directory (or ID6_POSIX_BIN)")
    parser.add_argument("--cxx-prefix", type=Path, default=os.environ.get("ID6_CXX_PREFIX"), help="optional matching Cygwin cc1plus directory for the GCC driver")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("this runner selects native Windows Go and Cygwin host tools")
    if args.posix_bin is None:
        parser.error("set --posix-bin to the installed Cygwin bin directory")
    folder = args.posix_bin.resolve()
    required = {name: folder / name for name in ("bash.exe", "cygpath.exe", "gcc.exe", "x86_64-w64-mingw32-gcc.exe")}
    for name, binary in required.items():
        if not binary.is_file():
            parser.error("missing required tool: " + str(binary))
    if args.cxx_prefix is not None:
        frontend = args.cxx_prefix.resolve()
        if not (frontend / "cc1plus.exe").is_file():
            parser.error("--cxx-prefix must contain the matching Cygwin cc1plus.exe")
        posix_frontend = subprocess.check_output([str(required["cygpath.exe"]), "-u", str(frontend)], text=True).strip()
        cxx = '/usr/bin/gcc -B"' + posix_frontend + '/"'
    elif (folder / "g++.exe").is_file():
        cxx = "/usr/bin/g++"
    else:
        parser.error("install Cygwin gcc-g++ or pass --cxx-prefix for its matching frontend")
    env = os.environ.copy()
    env["PATH"] = str(folder) + os.pathsep + env["PATH"]
    mingw = required["x86_64-w64-mingw32-gcc.exe"].as_posix()
    env.update(CC='"' + mingw + '"' if " " in mingw else mingw,
               HOST_CC=str(required["gcc.exe"]), CGO_ENABLED="1")
    # A Windows Python parent is not tracked by Cygwin's getppid(). Launch the
    # continuous-worker tests through Cygwin Bash AND Cygwin Python instead.
    posix_root = subprocess.check_output([str(required["cygpath.exe"]), "-u", str(ROOT)], text=True).strip()
    transport_python = subprocess.check_output([str(required["cygpath.exe"]), "-u", sys.executable], text=True).strip()
    # The transport simulator deliberately delays replies by 30 ms. Cygwin's
    # Python sleep can overshoot that enough to invalidate its pacing budget.
    # Keep the same collector binary and 215 ms limit, using Windows Python
    # for this simulator and Cygwin Python for parent-sensitive worker tests.
    commands = [
        [str(required["bash.exe"]), "--noprofile", "--norc", "-c", 'export PATH=/usr/bin:/bin; cd "$1" && /usr/bin/python3 --version && make -C native CC=/usr/bin/gcc "CXX=$3" "TRANSPORT_PYTHON=$2" test', "id6-tests", posix_root, transport_python, cxx],
        ["go", "test", "-race", "./..."],
        ["go", "vet", "./..."],
        [sys.executable, "-m", "unittest", "discover", "-s", "tools", "-p", "test_*.py"],
        ["node", "tools/test_trust_flow.js"],
        ["node", "--check", "web/app.js"],
    ]
    print("Host C++:", cxx, flush=True)
    print("Go CC:", env["CC"], flush=True)
    print("File-operations HOST_CC:", env["HOST_CC"], flush=True)
    print("Transport simulator Python:", sys.executable, flush=True)
    for command in commands:
        print("Running", " ".join(command), flush=True)
        subprocess.run(command, cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    main()
