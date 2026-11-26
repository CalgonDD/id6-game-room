#!/usr/bin/env python3
"""Validate locally built QNX artifacts and stage them for the desktop installer."""

import argparse
from pathlib import Path
from shutil import copyfile
import struct
from xml.etree import ElementTree
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
# The Game Room installer. The plugin and programs come from the native build in ../source/native; game data files go to payload/games/<Game>/ (installed on the hard drive).
APPS = (
    ("GameRoom", "freedoom-qnx", "220"),   # every game in one entry (2026-11-20): PC games and consoles
)

# Files an app ships besides the usual six: (app, payload name, source relative to the repository root).
EXTRA_FILES = tuple(
    # Retro's bundled homebrew (freely licensed; LICENSES.md lists each game's licence and source).
    ("games/Retro", f.name, f"../third_party/nes_homebrew/{f.name}")
    for f in sorted((Path(__file__).resolve().parents[2] / "third_party/nes_homebrew").iterdir())
    if f.is_file() and f.suffix.lower() in (".nes", ".md")
) + tuple(
    # Retro's Game Boy / Color homebrew (licences in LICENSES-GameBoy.md).
    ("games/Retro", "LICENSES-GameBoy.md" if f.name == "LICENSES.md" else f.name, f"../third_party/gb_homebrew/{f.name}")
    for f in sorted((Path(__file__).resolve().parents[2] / "third_party/gb_homebrew").iterdir())
    if f.is_file() and f.suffix.lower() in (".gb", ".gbc", ".md")
) + (
    # Retro's second program: the Game Boy / Color emulator (Gambatte), built next to nes-qnx.
    ("GameRoom", "nes-qnx", "../source/native/out/nes-qnx"),   # the console emulators, next to freedoom-qnx
    ("GameRoom", "gb-qnx", "../source/native/out/gb-qnx"),
    ("GameRoom", "psp-qnx", "../source/native/out/psp-qnx"),
    ("GameRoom", "ppsspp_libretro.so", "../source/native/out/ppsspp_libretro.so"),
    # PSP v25: GPU driver app hints, read by the car driver from app/native/powervr.ini relative to the working
    # directory (psp-qnx changes into /mnt/data/ID6Games/PSP before starting the game).
    ("games/PSP", "app/native/powervr.ini", "../source/native/psp/powervr.ini"),
    ("GameRoom", "ps1-qnx", "../source/native/out/ps1-qnx"),  # candidate with QNX bytewise dynarec lookup
    # Game Room's other engines, next to freedoom-qnx (which shows the list and becomes them).
    ("GameRoom", "tyrian-qnx", "../source/native/out/tyrian-qnx"),
    ("GameRoom", "quake-qnx", "../source/native/out/quake-qnx"),
) + (
    ("games/LibreQuake", "pak0.pak", "../third_party/librequake/lite/id1/pak0.pak"),
    ("games/LibreQuake", "pak1.pak", "../third_party/librequake/lite/id1/pak1.pak"),
    ("games/Freedoom", "freedoom1.wad", "../third_party/freedoom/freedoom-0.13.0/freedoom1.wad"),
    ("games/Freedoom", "freedoom2.wad", "../third_party/freedoom/freedoom-0.13.0/freedoom2.wad"),
) + tuple(
    # Tyrian 2.1's data: every file of the release except its DOS programs and overlays.
    ("games/Tyrian", f.name, f"../third_party/tyrian21/x/tyrian21/{f.name}")
    for f in sorted((Path(__file__).resolve().parents[2] / "third_party/tyrian21/x/tyrian21").iterdir())
    if f.is_file() and f.suffix.lower() not in (".exe", ".ovl", ".com", ".bat")
)

# Include image IDs selected at runtime, not only the XML's initial values.
EXTRA_FILES += tuple(
    ("games/PSP", f.relative_to(Path(__file__).resolve().parents[2] / "third_party/ppsspp-assets").as_posix(),
     "../third_party/ppsspp-assets/" + f.relative_to(Path(__file__).resolve().parents[2] / "third_party/ppsspp-assets").as_posix())
    for f in sorted((Path(__file__).resolve().parents[2] / "third_party/ppsspp-assets").rglob("*")) if f.is_file()
)
RUNTIME_IMAGES = {
    "GameRoom": {57800},  # one blank image; the games draw on their own screen layer
}


def require_arm(path, elf_type):
    with path.open("rb") as binary:
        head = binary.read(20)
    if head[:6] != b"\x7fELF\x01\x01" or len(head) != 20:
        raise ValueError(f"{path}: expected a little-endian ELF32 file")
    if struct.unpack_from("<HH", head, 16) != (elf_type, 40):
        raise ValueError(f"{path}: unexpected ARM ELF type")


def reject_unexpected_payload(payload, expected):
    """Fail closed: go:embed would otherwise retain a stale disabled emulator."""
    present = {p.relative_to(payload).as_posix() for p in payload.rglob("*") if p.is_file()}
    unexpected = sorted(present - expected)
    if unexpected:
        raise ValueError("Unexpected payload files (not removed automatically): " + ", ".join(unexpected))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="verify the staged payload")
    args = parser.parse_args()
    native = ROOT.parent / "source" / "native"
    payload = ROOT / "payload"
    copies = []
    for app, worker, connector in APPS:
        destination = payload / app
        plugin = destination if args.check else native / "plugin"
        binary = destination if args.check else native / "out"
        manifest = plugin / f"{app}.manifest"
        description = plugin / f"{app}.xml"
        images = plugin / f"{app}_ALL_Images.zip"
        texts = plugin / f"{app}_ALL_Texts.zip"
        library = binary / f"lib{app}.so"
        executable = binary / worker
        config = ElementTree.parse(manifest)
        if config.findtext("name") != app or config.findtext("config/dependencies/pluginAppConnector/id") != connector:
            raise ValueError(f"{manifest}: app name or connector mismatch")
        if config.findtext("config/parameters/TextDBFile") != "{pluginDir}/" + texts.name:
            raise ValueError(f"{manifest}: missing or mismatched TextDBFile parameter")
        ids = {f'{node.attrib["imageId"]}.png' for node in ElementTree.parse(description).iter("imageIdModel")}
        ids |= {f"{identifier}.png" for identifier in RUNTIME_IMAGES[app]}
        with ZipFile(images) as archive:
            if archive.testzip() is not None or not ids <= set(archive.namelist()):
                raise ValueError(f"{images}: invalid archive or missing initial/runtime image")
        with ZipFile(texts) as archive:
            if archive.testzip() is not None or not archive.namelist():
                raise ValueError(f"{texts}: invalid or empty archive")
        require_arm(library, 3)
        require_arm(executable, 2)
        copies.extend((path, destination / path.name) for path in (manifest, description, images, texts, library, executable))
    for app, name, source in EXTRA_FILES:
        staged = payload / app / name
        original = (ROOT / source).resolve()
        if name.endswith("-qnx"):
            require_arm(staged if args.check else original, 2)
        elif name == "ppsspp_libretro.so":
            require_arm(staged if args.check else original, 3)
        if args.check:
            if not staged.is_file() or not original.is_file() or staged.read_bytes() != original.read_bytes():
                raise ValueError(f"{staged}: missing or differs from {original}; stage again")
        else:
            if not original.is_file():
                raise ValueError(f"{original}: missing (see third_party/)")
            copies.append((original, staged))
    helper = payload / "tools" / "gameroom-fileops"
    executable = helper if args.check else native / "out" / helper.name
    require_arm(executable, 2)
    copies.append((executable, helper))
    if args.check:
        for staged, _ in copies:
            folder = native / ("plugin" if staged.suffix in (".xml", ".manifest", ".zip") else "out")
            current = folder / staged.name
            if not current.is_file() or current.read_bytes() != staged.read_bytes():
                raise ValueError(f"{staged}: differs from the current native build; stage again")
    if not args.check:
        for source, destination in copies:
            destination.parent.mkdir(parents=True, exist_ok=True)
            copyfile(source, destination)
    expected = set()
    for app, worker, _ in APPS:
        expected.update(f"{app}/{name}" for name in (
            f"{app}.manifest", f"{app}.xml", f"{app}_ALL_Images.zip",
            f"{app}_ALL_Texts.zip", f"lib{app}.so", worker))
    expected.update(f"{app}/{name}" for app, name, _ in EXTRA_FILES)
    expected.add("tools/gameroom-fileops")
    reject_unexpected_payload(payload, expected)
    print(f"Verified all {len(APPS)} app payloads and the ARM helper.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, ElementTree.ParseError) as error:
        raise SystemExit(f"{error}\nSee docs/BUILD.md for the required build inputs.") from None
