# Building Game Room

There are three parts. Each one can be checked against the release by hash (`payload-SHA256SUMS.txt` on the release page).

| Part | Output | Built with |
| --- | --- | --- |
| 1. PSP core | `ppsspp_libretro.so` | Docker, free QNX 6.5 GCC 8.5 toolchain image |
| 2. Everything else on the head unit | `libGameRoom.so`, `freedoom-qnx`, `tyrian-qnx`, `quake-qnx`, `nes-qnx`, `gb-qnx`, `ps1-qnx`, `psp-qnx`, `gameroom-fileops` | QNX SDP 6.5 (`qcc`, GCC 4.4.2) |
| 3. Windows installer | `ID6 Game Room Installer.exe` | Go 1.27 |

## 0. Get the sources

```sh
sh third_party/fetch_third_party.sh --psp
```

This clones every upstream project at the exact commit used. It also downloads Freedoom, LibreQuake and Tyrian 2.1 and checks their SHA-256. With `--psp` it also clones PPSSPP v1.17.1 with its submodules (about 2 GB) and applies `third_party/patches/ppsspp-1.17.1-qnx.diff`. Leave out `--psp` if you only need parts 2 and 3.

## 1. PSP core (Docker)

The core is built with GCC 8.5 for QNX 6.5 from [luka-dev/qnx65-armv7-toolchain](https://github.com/luka-dev/qnx65-armv7-toolchain). Build its image once, at the commit used for the release:

```sh
git clone https://github.com/luka-dev/qnx65-armv7-toolchain && cd qnx65-armv7-toolchain
git checkout baa45224f18ae6b13c2e9c77c6663bb8181eb6a1
docker build --platform=linux/amd64 --build-arg BASE=base-8.5 --target base-env -t qnx65-armv7-toolchain:8.5 .
```

Then, from this repository:

```sh
sh source/native/psp/build_psp_core.sh
```

It builds FFmpeg (decoders only, no assembly) and then PPSSPP's libretro core. The stripped result goes to `source/native/out/ppsspp_libretro.so`. The recipes are in `source/native/psp/`:
- `build-ffmpeg.sh` and `configure-core.sh`: the exact flags;
- `qnx_c_types.h` and `qnx_cpp_compat.h`: QNX 6.5 header gaps;
- `libretro-exports.map`: the symbols exported;
- `powervr.ini`: the GPU driver settings installed next to PSP games.

What the patch changes in PPSSPP:
- QNX platform support: JIT instruction-cache flush, shared memory, no unaligned ARM pair fusion.
- PowerVR SGX544 fixes: GPU detection, a GLES 2 blend fallback, alpha-test precision, lighting NaN guards.
- A disk shader cache that saves as you play.
- Auto frameskip support.
- Timing counters that the PSP front end logs.

## 2. Head-unit programs (QNX SDP 6.5)

You need:
- **QNX SDP 6.5.0** with `qcc` for `gcc_ntoarmv7le` (GCC 4.4.2). It is BlackBerry's and is not in this repository.
- **The two HMI framework libraries from your own head unit**: `libAppFramework.A.so` and `libCoreFramework.A.so`, from `/fs/sda0/opt/hmi/ID5/lib/`.
  - Copy them into `source/native/frameworks/`, or set `NBTEVO_FRAMEWORKS_DIR`. They're BMW's: never commit them (`.gitignore` excludes them).
  - Copy them over the same SSH access the installer uses, e.g. `ssh root@<head unit> cat /fs/sda0/opt/hmi/ID5/lib/libAppFramework.A.so > libAppFramework.A.so`. That is how the installer itself copies files byte for byte.

```sh
export QNX_HOST=/opt/qnx650/host/linux/x86 QNX_TARGET=/opt/qnx650/target/qnx6   # or the win32 paths on Windows
export PATH="$QNX_HOST/usr/bin:$PATH"
sh source/native/build_gameroom.sh
```

Outputs go to `source/native/out/`.

The Game Room page's art and texts (`source/native/plugin/GameRoom_ALL_*.zip`) are already built. To regenerate them, run `python tools/make_gameroom.py` from `source/native/`. You need Python 3 with Pillow. `python tools/make_texts.py` regenerates the texts zip.

Host tests for the game front ends are in `source/native/tests/`. Each test file names the source it covers.

## 3. Windows installer (Go)

```sh
cd games_installer
python tools/stage_payload.py          # copies parts 1 and 2 and the game data into payload/, and checks them
python tools/stage_payload.py --check
go test ./...
ID6_BUILD_LABEL="Game Room v34" python tools/build_release.py ../dist
```

Expected test result: everything passes except the two symlink tests, which can't create symlinks on Windows without Developer Mode.

`build_release.py` builds with `-trimpath -buildvcs=false -ldflags="-s -w -H windowsgui"`. The release used the label `Game Room v34` and Go 1.27.4. With the same payload files, the `.exe` matches the release byte for byte (see [SAFETY.md](SAFETY.md)).

## Layout

| Path | What's there |
| --- | --- |
| `source/native/src/gameroom.cpp` | The Game Room page in My Vehicle: launches the game program, owns nothing else. |
| `source/native/src/freedoom_worker.c` | The game list, Freedoom, and the shared screen, input and sound code. |
| `source/native/src/tyrian_worker.c`, `quake_worker.c` | Front ends for Tyrian and Quake. |
| `source/native/src/nes_worker.c` | One libretro front end, built three times: NES, Game Boy (`RETRO_CORE_GB`) and PlayStation (`RETRO_CORE_PS1`). |
| `source/native/src/psp_worker.c` | The PSP front end. It loads `ppsspp_libretro.so`, owns the GLES2 surface, and runs the memory and freeze watchdogs. |
| `source/native/src/xpad.h`, `gip.h` | The wired Xbox controller driver (GIP over USB). |
| `source/native/sdk/bmw_list.h` | The reconstructed HMI plugin interface (declarations only). |
| `games_installer/` | The installer. `install.go`/`games.go` hold the on-car steps; `owngames.go`/`psp_games.go` check your own games; `web/` is the interface. |
