# Third-party components

Game Room's own code is under the PolyForm Noncommercial License 1.0.0 (see [LICENSE.md](LICENSE.md) and [LICENSING.md](LICENSING.md)). The programs it installs on the head unit are built together with the projects below. Where a program links GPL code, that program as distributed is covered by the GPL.

The complete corresponding source for every binary in the installer is:
- this repository;
- the upstream projects at the commits below, fetched by [`third_party/fetch_third_party.sh`](third_party/fetch_third_party.sh);
- our PPSSPP patch, [`third_party/patches/ppsspp-1.17.1-qnx.diff`](third_party/patches/ppsspp-1.17.1-qnx.diff).

A single archive of all of it, `GameRoom-v34-complete-source.tar.xz`, is attached to each release.

## Engines and emulators (compiled into the head-unit programs)

| Program | Upstream project | Version used | Licence |
| --- | --- | --- | --- |
| `freedoom-qnx` | [doomgeneric](https://github.com/ozkl/doomgeneric) | `e257a095` | GPL-2.0 |
| `freedoom-qnx` (music) | [Chocolate Doom](https://github.com/chocolate-doom/chocolate-doom) OPL player and MIDI reader, copied into `source/native/src/opl/` | `35fb1372` (3.1.0) | GPL-2.0 |
| `freedoom-qnx` (music) | Nuked OPL3, in `source/native/src/opl/` | as shipped with Chocolate Doom | GPL-2.0-or-later |
| `tyrian-qnx` | [OpenTyrian](https://github.com/opentyrian/opentyrian) | `26e40a20` | GPL-2.0 |
| `quake-qnx` | [quakegeneric](https://github.com/erysdren/quakegeneric) | `13052102` | GPL-2.0 |
| `quake-qnx` (sound) | [id Software Quake](https://github.com/id-Software/Quake) WinQuake mixer, copied into `source/native/src/quake/` | `bf4ac424` | GPL-2.0 |
| `nes-qnx` | [FCEUmm](https://github.com/libretro/libretro-fceumm) | `77468e56` | GPL-2.0 |
| `gb-qnx` | [Gambatte](https://github.com/libretro/gambatte-libretro) | `77f2567d` | GPL-2.0 |
| `ps1-qnx` | [PCSX ReARMed](https://github.com/libretro/pcsx_rearmed) | `7b0e8f8f` | GPL-2.0 |
| `ppsspp_libretro.so` | [PPSSPP](https://github.com/hrydgard/ppsspp) v1.17.1, with [our QNX patch](third_party/patches/ppsspp-1.17.1-qnx.diff) | `d479b74e` | GPL-2.0-or-later |
| `ppsspp_libretro.so` | FFmpeg (PPSSPP's `ffmpeg` submodule), video and audio decoders only, built without assembly | PPSSPP v1.17.1 submodule | LGPL-2.1-or-later |
| every program | 8x8 bitmap font by Daniel Hepper ([`third_party/font8x8`](third_party/font8x8)) | — | Public domain |

PPSSPP's own bundled libraries (libchdr, libpng, libzip, zstd, snappy, SPIRV-Cross, glslang, xxhash, rcheevos and others) keep their own licences. Their licence texts are in [`games_installer/third-party/PPSSPP/`](games_installer/third-party/PPSSPP) and ship inside the installer download.

## Game data bundled with the installer

| Data | Source | Licence |
| --- | --- | --- |
| Freedoom 0.13.0 (`freedoom1.wad`, `freedoom2.wad`) | [freedoom.github.io](https://freedoom.github.io) | BSD-3-Clause |
| LibreQuake v0.09-beta "lite" (`pak0.pak`, `pak1.pak`) | [LibreQuake](https://github.com/lavenderdotpet/LibreQuake) | BSD-3-Clause (assets), GPL-2.0 (QuakeC `progs.dat`) |
| Tyrian 2.1 data | [camanis.net](https://camanis.net/tyrian/tyrian21.zip), the mirror OpenTyrian uses | Freeware: released free by its author, Jason Emery, and redistributed by OpenTyrian and Linux distributions |
| NES homebrew: Nova the Squirrel, Thwaite, Concentration Room, RHDE: Furniture Fight | see [`third_party/nes_homebrew/LICENSES.md`](third_party/nes_homebrew/LICENSES.md) | GPL-3.0-or-later or FSFAP, with links to each game's source |
| Game Boy homebrew: Libbet and the Magic Floor, µCity | see [`third_party/gb_homebrew/LICENSES.md`](third_party/gb_homebrew/LICENSES.md) | µCity: GPL-3.0-or-later code, CC BY-SA 4.0 art and music. Libbet: zlib. Both with source links. |
| PPSSPP assets (UI atlases, free replacement PSP fonts, shaders, language files) | PPSSPP v1.17.1 `assets/` | GPL-2.0-or-later (no Sony firmware) |

## Windows installer

| Component | Licence |
| --- | --- |
| Go standard library | BSD-3-Clause |
| golang.org/x/crypto, golang.org/x/sys | BSD-3-Clause |
| github.com/jchv/go-webview2, github.com/jchv/go-winloader | MIT |
| ID6_apps_prerelease and idrive7-apps (the installer and plugin approach this project grew from) | MIT |

Their licence texts are in [`games_installer/third-party/`](games_installer/third-party) and [`games_installer/vendor/`](games_installer/vendor).

## Not included, never redistributed

- **BMW's HMI framework libraries** (`libAppFramework.A.so`, `libCoreFramework.A.so`). `libGameRoom.so` links against them, and the head unit already has them. To build, you copy them from your own car; see [docs/BUILDING.md](docs/BUILDING.md).
  - `source/native/sdk/bmw_list.h` is our own reconstructed header describing the interface (layouts and declarations only, no BMW code), written for interoperability.
- **QNX SDP 6.5**, BlackBerry's QNX compiler and system libraries. You need your own copy to build the head-unit programs other than the PSP core.
- **Game BIOS, firmware and commercial games.** You supply your own PlayStation BIOS and your own PlayStation and PSP games.
