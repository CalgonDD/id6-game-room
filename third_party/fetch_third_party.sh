#!/bin/sh
# Fetch every upstream project and free game data Game Room is built from, at the exact versions the
# released installer used, and check them. Run from anywhere; everything lands next to this script.
#
#   sh third_party/fetch_third_party.sh          # engines, emulators and game data
#   sh third_party/fetch_third_party.sh --psp    # also PPSSPP v1.17.1 (about 2 GB with submodules) + our patch
#
# Needs: git, curl, unzip, sha256sum, patch, sed. Works in Linux, WSL, macOS and Git Bash on Windows (long paths
# are switched on and line-ending conversion off for the clones; PPSSPP has deeply nested submodules and
# shell scripts that must keep Unix line endings).
set -eu
cd "$(dirname "$0")"

clone() {  # name url commit
    if [ -d "$1/.git" ]; then echo "have $1"; else git -c core.longpaths=true -c core.autocrlf=false clone --quiet "$2" "$1"; fi
    git -C "$1" checkout --quiet "$3"
    test "$(git -C "$1" rev-parse HEAD)" = "$3" || { echo "$1: wrong commit" >&2; exit 1; }
    echo "ok   $1 @ $3"
}

fetch() {  # url file sha256
    if [ ! -f "$2" ]; then curl -fL --retry 3 -o "$2.part" "$1" && mv "$2.part" "$2"; fi
    echo "$3  $2" | sha256sum -c - >/dev/null || { echo "$2: checksum mismatch" >&2; exit 1; }
    echo "ok   $2"
}

# Engines and emulators (all GPL-2.0 or compatible; see THIRD_PARTY_NOTICES.md).
clone doomgeneric    https://github.com/ozkl/doomgeneric.git            e257a09527062bfb986b6e5e0780839b6cbd5b71
clone opentyrian     https://github.com/opentyrian/opentyrian.git       26e40a2012903812f16aae5dd6af2be50b96b5cb
clone quakegeneric   https://github.com/erysdren/quakegeneric.git       13052102577c629650cf07a46151a4b6e1b19c3c
clone fceumm         https://github.com/libretro/libretro-fceumm.git    77468e568d9fe9c48d684ca911f1a3fe7259fdf7
clone gambatte       https://github.com/libretro/gambatte-libretro.git  77f2567d718b8973d02caa2daa8dce4953faf811
clone pcsx_rearmed   https://github.com/libretro/pcsx_rearmed.git       7b0e8f8fadfbce3d170d195b9d9eecaabfa08c8e
# Sources of files copied into source/native/src (OPL music player, Quake sound mixer); for reference only.
clone chocolate-doom https://github.com/chocolate-doom/chocolate-doom.git 35fb1372d10756ca27eca05665bd8a7cebc71c05
clone id-quake       https://github.com/id-Software/Quake.git           bf4ac424ce754894ac8f1dae6a3981954bc9852d

# Free game data bundled by the installer.
mkdir -p freedoom librequake tyrian21
fetch https://github.com/freedoom/freedoom/releases/download/v0.13.0/freedoom-0.13.0.zip \
      freedoom/freedoom-0.13.0.zip 3f9b264f3e3ce503b4fb7f6bdcb1f419d93c7b546f4df3e874dd878db9688f59
fetch https://github.com/lavenderdotpet/LibreQuake/releases/download/v0.09-beta/lite.zip \
      librequake/lite.zip 428e736b2f01d953e09a08c60bee975bdc4a0ac2219e97fa095c8af41754da83
fetch https://camanis.net/tyrian/tyrian21.zip \
      tyrian21/tyrian21.zip 7790d09a2a3addcd33c66ef063d5900eb81cc9c342f4807eb8356364dd1d9277
unzip -oq freedoom/freedoom-0.13.0.zip -d freedoom
unzip -oq librequake/lite.zip -d librequake
unzip -oq tyrian21/tyrian21.zip -d tyrian21/x
# nes_homebrew/, gb_homebrew/, font8x8/ and ppsspp-assets/ are already in this repository.

if [ "${1:-}" = --psp ]; then
    if [ ! -d ppsspp ]; then
        git -c core.longpaths=true -c core.autocrlf=false clone --quiet --branch v1.17.1 --recurse-submodules https://github.com/hrydgard/ppsspp.git ppsspp
    fi
    test "$(git -C ppsspp rev-parse HEAD)" = d479b74ed9c3e321bc3735da29bc125a2ac3b9b2 || { echo "ppsspp: wrong commit" >&2; exit 1; }
    if [ ! -f ppsspp/Common/QNXJitCache.h ]; then sh patches/apply_ppsspp_patch.sh ppsspp; fi
    echo "ok   ppsspp v1.17.1 + Game Room QNX patch"
fi
echo "All third-party sources and game data are in place."
