# Is it safe? What the installer does, and how to check it yourself

## 1. Check the download is the published one

The release installer is `ID6-Game-Room-Installer-v33.2-Windows-x64.exe`. Its SHA-256 is:

```
0244e8984a8e8d90a961dd25a8002e66fc3e6a838b3481ca27036e7111d50e0c
```

In PowerShell, `Get-FileHash .\ID6-Game-Room-Installer-v33.2-Windows-x64.exe` must print that value (in capitals). Some chat apps flag or rewrite `.exe` files, so download it from GitHub directly.

## 2. Rebuild the installer and compare

The Windows installer is a plain Go program. Rebuilding it from this repository gives a byte-identical `.exe`, so you can confirm the release contains nothing else.

1. Install Go 1.27.4 (the version recorded inside the release; `go version -m <exe>` shows it).
2. Put the head-unit files in `games_installer/payload/`, in the layout listed in `payload-SHA256SUMS.txt`. That file is attached to the release, and every file's hash is listed there. Either:
   - build those files yourself ([BUILDING.md](BUILDING.md)), or
   - take them from the release's `payload` archive.
3. In `games_installer/`, run:

   ```sh
   GOOS=windows GOARCH=amd64 CGO_ENABLED=0 go build -mod=vendor -trimpath \
     "-ldflags=-s -w -H windowsgui -X 'main.buildLabel=Game Room v33.2'" -o rebuilt.exe .
   ```

4. `sha256sum rebuilt.exe` gives `0244e898…0e0c`, the same as the release. This was checked before publishing.

## 3. Rebuild the head-unit files and compare

[BUILDING.md](BUILDING.md) explains how to build each head-unit file. The released head-unit programs were built from this repository with `build_gameroom.sh`. Building them a second time gave these results, checked before publishing:

| File | Rebuilt from this repository |
| --- | --- |
| `libGameRoom.so`, `freedoom-qnx`, `tyrian-qnx`, `nes-qnx`, `gb-qnx`, `psp-qnx`, `gameroom-fileops` | Byte-identical. |
| `quake-qnx`, `ps1-qnx` | Identical code. Only the build date and time the compiler stamps into the file differ (in `quake-qnx` this can also change the file length by a few bytes). |
| `ppsspp_libretro.so` | Identical apart from 7 bytes: the build date and time libpng stamps into its error text. Built with `build_psp_core.sh` in the same toolchain image. |
| Game data (Freedoom, LibreQuake, Tyrian, homebrew) | Identical to the official downloads (SHA-256 checked by `fetch_third_party.sh`). |

## 4. What the installer does

**On your PC**
- It opens a local window (WebView2). It never connects to the internet. The only network connection it makes is SSH to the head unit over the ENET cable, and it refuses other links for car operations.
- It saves the car's address and display setting only if you tick **Remember**. The SSH password is then stored encrypted with Windows DPAPI (only your Windows account can decrypt it), never as plain text.
- It keeps an action log in `%AppData%\ID6AppInstaller\actions.log`. The password is never written to it.

**On the head unit** (only after you press Install, Uninstall, Your games or Diagnostics)
- It connects with the root SSH password you type. It shows the head unit's SSH key fingerprint and asks you to trust it; if the key changes later, it stops.
- It puts the small Game Room plugin in `/var/opt/hmi/data/ID5/Plugins/GameRoom/` on the flash, and the game data on the hard drive in `/mnt/data/ID6Games/`.
- To write those folders it briefly makes them writable, then sets them back to read-only. Restarting iDrive also does that.
- It backs up anything it replaces, checks every copied file by size and hash, and can undo the install (**Uninstall**).
- When you add your own games, it checks each one (type, size, header) and copies only that file.
- **Diagnostics** only reads: logs, settings files and, if you tick it, crash dumps. Everything goes into a zip on your PC. It never copies the car's own libraries. An iDrive crash dump is a copy of the iDrive's memory and can include personal data (contacts, addresses), so share crash dumps only privately.
- It never restarts the car and never changes BMW's own system files.

**While you play**
- The games draw on their own screen layer, read the Xbox controller from USB, and play sound through the head unit's audio.
- Game Room doesn't talk to the car's control units (ECUs) at all.
- Pressing **BACK** on the iDrive controller stops the game and returns to iDrive.
- The PSP emulator checks free memory before it starts and while it runs, and stops if memory gets low.

## 5. What is not included

- No BMW software, no QNX libraries, no game BIOS or firmware, no commercial games.
- You supply your own PlayStation BIOS and your own PlayStation and PSP games.
- [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) lists everything that *is* included, with licences and sources.

## 6. Known limits

- Tested on one car (NBT EVO ID6, firmware `NBTevo_V20153I`, 1280x480 screen). The installer checks the firmware and screen first.
- Demanding PSP games draw about 15 frames per second to stay at full game speed.
- Use it only while parked. This is unofficial software; you install it at your own risk.
