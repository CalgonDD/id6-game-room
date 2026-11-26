# Game Room for BMW NBT EVO (ID6)

Game Room is one app in the iDrive **My Vehicle** menu that plays games on the head unit's own screen with a wired Xbox controller:

| Game or system | What runs it | Games included |
| --- | --- | --- |
| Freedoom (Doom engine) | doomgeneric + Chocolate Doom's OPL music | Freedoom Phase 1 and Phase 2 |
| Tyrian 2.1 | OpenTyrian | Tyrian 2.1 (freeware) |
| Quake engine | quakegeneric (id Software's GPL Quake) | LibreQuake |
| NES | FCEUmm | Four free homebrew games |
| Game Boy / Color | Gambatte | Two free homebrew games |
| PlayStation | PCSX ReARMed | None. Add your own discs and BIOS. |
| PSP | PPSSPP 1.17.1 with QNX changes | None. Add your own games. |

Everything runs on the head unit itself (QNX 6.5, ARM Cortex-A15, PowerVR SGX544). Nothing is streamed from a phone.

> **Beta.** Game Room has been tested on one car and with a small number of games. Expect some games not to start, run slowly or look wrong, and other cars or firmware may not work at all. Please report what you find (see [Help test the beta](#help-test-the-beta)).

> **Use it only while parked.** This is unofficial software, not made or approved by BMW. Installing it changes files on your head unit, and you do so at your own risk.

## Download and check it

Get `ID6-Game-Room-Installer-v33.2-Windows-x64.exe` from the [Releases](../../releases) page. Before running it, check it is the file built from this source. In PowerShell:

```powershell
Get-FileHash .\ID6-Game-Room-Installer-v33.2-Windows-x64.exe -Algorithm SHA256
```

The result must be `0244E8984A8E8D90A961DD25A8002E66FC3E6A838B3481CA27036E7111D50E0C`. If it is not, don't run it.

You don't have to take that on trust: [docs/SAFETY.md](docs/SAFETY.md) explains how to rebuild the installer from this repository and get the same file, and what it does to your car.

## What you need

- A BMW with an **NBT EVO ID6** head unit. Game Room has been tested on one car: firmware `NBTevo_V20153I`, 1280x480 non-touch screen. Other firmware may work, but hasn't been tested.
- A Windows 10/11 x64 PC with Microsoft Edge WebView2 (already part of Windows 11).
- An **ENET cable** (OBD to Ethernet) and the head unit's temporary **root SSH access** with its password. The installer doesn't unlock the head unit; it needs SSH access that already works.
  - You can enable temporary SSH with the [Bavarian SSH Client](https://ssh.bavarian-aa.uk/). It's a separate tool, not part of this project, and many NBT EVO projects use it.
  - The unlock lasts only until the head unit restarts, so enable it again each time before you use the installer.
- A **wired Xbox controller** (Xbox One / Series) plugged into the car's USB port.
- For PlayStation games, your own BIOS dump. For PlayStation and PSP, your own game discs. None are included, and none ever will be.

## Install

1. Park, keep iDrive awake, and connect the PC to the car with the ENET cable. Enable temporary SSH (for example with the [Bavarian SSH Client](https://ssh.bavarian-aa.uk/)).
2. Run the installer and press **Find car**. Enter the root SSH password. Check the SSH fingerprint it shows before you trust it.
3. Press **Install**. The installer backs up anything it replaces, copies Game Room to the head unit and checks every file afterwards.
4. Restart iDrive (hold the volume/power knob) while parked. Game Room appears under **My Vehicle**.
5. To add your own games, use **Your games** in the installer. Restarting iDrive closes the temporary SSH access, so enable it again first. Supported files:
   - PSP: `.iso`, `.cso`, `.pbp`, `.chd`
   - PlayStation: `.cue`/`.bin`, `.chd`, plus the BIOS
   - NES: `.nes`
   - Game Boy / Color: `.gb`, `.gbc`

**Uninstall** in the same installer removes Game Room and puts back what it replaced.

## Controls

| Where | Controls |
| --- | --- |
| Game list | D-pad and **A** to pick a game. **B** leaves the list. |
| PC games (Freedoom, Tyrian, Quake) | Left stick moves, **RT** fires, **Menu** opens the game's menu (save, load, quit). Answer questions with **A** (yes) or **B** (no). |
| Consoles | **A / B / X / Y** are the console's buttons, **Menu** is Start, **View** is Select. The **Xbox** button saves state, loads, or switches game. |
| PSP | Left stick is the analog nub. **LB/LT** and **RB/RT** are L and R. |
| Any game | **BACK** on the iDrive controller ends the game. |

### Your own buttons

NES, Game Boy, PS1 and PSP games can each have their own button layout, saved on the car for that game:

- **NES, Game Boy, PS1:** press the **Xbox** button in the game and choose **Change buttons**.
- **PSP:** in the game list, highlight the PSP game and press **Y**.

Pick a console button, press **A**, then press the Xbox button you want for it. If that Xbox button was already in use, the two swap. **Reset to default** puts the usual layout back; **B** or **Done** saves and goes back. The d-pad and sticks always keep their usual jobs.

## How well it runs

- **Freedoom, Tyrian, Quake, NES, Game Boy:** full speed.
- **PlayStation:** full speed in the games tested.
- **PSP:** demanding 3D games run at about full game speed but draw roughly 15 frames per second. GTA: Liberty City Stories was the test game.
  - The first time you play a game there are a few short pauses while it builds its graphics effects. After that it plays smoothly.

## Help test the beta

Open an [issue](../../issues) for each game or car you try, whether it works or not. Include:

- **Car:** model and year, head-unit firmware (the installer shows it, e.g. `NBTevo_V20153I`) and screen size.
- **Game:** system, title and region (e.g. PSP, GTA: Liberty City Stories, ULES00151), and the file type (`.iso`, `.cso`, `.chd`, ...).
- **What happened:**
  - Did it start?
  - Speed, sound and picture.
  - Any freeze, or iDrive restarting.
  - A photo of the screen helps with picture problems.
- **Diagnostics:** if something went wrong, pull diagnostics in the installer straight afterwards.
  - Before attaching the zip to a public issue, untick **Crash dumps of the games and the iDrive**. An iDrive crash dump is a copy of the iDrive's memory, which can include your contacts, addresses and phone data. Offer to share it privately instead.
  - Never post your VIN or the SSH password.

## Source and building

| Folder | What's in it |
| --- | --- |
| [`games_installer/`](games_installer) | The Windows installer (Go, WebView2 interface). |
| [`source/native/`](source/native) | Everything that runs on the head unit: the Game Room page, the iDrive front end for each engine and emulator, the PSP front end, and the art and text generators. |
| [`third_party/`](third_party) | The free game data bundled with the installer, our PPSSPP patch, and a script that fetches every upstream project at the exact version used. |

[docs/BUILDING.md](docs/BUILDING.md) explains how to build everything.

## Licence

**Game Room is free and must stay free. If you paid for it, you were sold something that is given away here.**

- Our own code is under the [PolyForm Noncommercial License 1.0.0](LICENSE.md):
  - You may use, change and share it free.
  - You may not sell it or bundle it into anything that is sold.
- The open-source engines and emulators keep their own licences. Under the GPL, the game programs built with them, as distributed, are GPL.
- [LICENSING.md](LICENSING.md) explains exactly which part is under which licence.
- [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) lists every component, its licence and where its source is.

BMW, iDrive, PlayStation, PSP, Xbox, Doom, Quake and Tyrian are trademarks of their owners. This project isn't affiliated with or endorsed by any of them.
