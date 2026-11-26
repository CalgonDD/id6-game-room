# Licensing: free to use, not for sale

Game Room is free. **If you paid for it, you were sold something that is given away here:** https://github.com/CalgonDD/id6-game-room

## Our own code: PolyForm Noncommercial 1.0.0

Everything in this repository that we wrote is under the [PolyForm Noncommercial License 1.0.0](LICENSE.md). That covers:
- the Windows installer;
- the Game Room page;
- the PSP front end;
- the build scripts, art and tools;
- the documentation.

The licence says you **may**:
- use it, including in your own car;
- copy it and share it free;
- change it and share your changed version free.

You **may not** sell it, bundle it into anything you sell, or use it for any other commercial purpose.

Anyone who shares it or a changed version must pass on the licence and this line:

```
Required Notice: Copyright 2026 CalgonDD (https://github.com/CalgonDD/id6-game-room)
```

## Exceptions the licence cannot override

These parts come from other people or are tied to GPL code. Their own licences apply, and some of them allow selling:

1. **The game programs.** Each of these is built together with a GPL engine or emulator:
   - `freedoom-qnx`, `tyrian-qnx`, `quake-qnx`, `nes-qnx`, `gb-qnx`, `ps1-qnx`.

   The GPL requires each of those programs, as a whole, to be under the GPL. So our source files that are compiled into them are also available under GPL-2.0-or-later when used as part of those programs:
   - in `source/native/src/`:
     - `freedoom_*`, `tyrian_*`, `quake_*`, `nes_*`, `ps1_*`;
     - `car_audio.h`, `xpad.h`, `gip.h`, `launcher.h`;
     - `gr_pad.c`, `gr_show*.h`, `gr_log.h`, `hmi_trace_*.h`;
     - the `opl/`, `quake/`, `tyrian/`, `sdl_stub/` and `ps1_compat/` folders.

   The GPL allows anyone to sell copies of GPL programs, as long as they give buyers the full source.
2. **Our PPSSPP patch** (`third_party/patches/`) changes GPL code, so it is GPL-2.0-or-later like PPSSPP.
3. **Code from the projects Game Room grew from** stays under their MIT licences. Those projects are ID6_apps_prerelease ("ID6 Apps contributors") and idrive7-apps. See:
   - `games_installer/LICENSE-upstream-MIT.txt`;
   - `source/LICENSE-upstream-MIT.txt`;
   - `games_installer/third-party/`.
4. **Third-party projects and game data** keep their own licences. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

What this means in practice:
- Nobody may sell or bundle our installer, the Game Room page, the PSP front end, our art or our documentation.
- The open-source engines and emulators stay open source, as their authors intended.

## The name

Please don't call a changed version "Game Room", and don't present it as this project, so people can always tell where the free original is.

## Not legal advice

This file explains our intent in plain words. The licence texts are what count.
