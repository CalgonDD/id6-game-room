#!/bin/sh
set -eu
# Game Room only: builds the Game Room page (libGameRoom.so) and every game program it starts, from
# this repository's source plus the upstream projects fetched by third_party/fetch_third_party.sh.
# The PSP front end (psp-qnx) is the last step. The PSP emulator core itself (ppsspp_libretro.so) is built
# in Docker: see docs/BUILDING.md and psp/build_psp_core.sh.
#
# Needs: QNX SDP 6.5.0 (qcc, gcc 4.4.2 for armv7le) and the two HMI framework libraries copied from your
# own head unit into source/native/frameworks/ (they are BMW's and are never in this repository; see docs/BUILDING.md).
cd "$(dirname "$0")"
export QNX_HOST="${QNX_HOST:-/opt/qnx650/host/linux/x86}"
export QNX_TARGET="${QNX_TARGET:-/opt/qnx650/target/qnx6}"
export QNX_CONFIGURATION="${QNX_CONFIGURATION:-/etc/qnx}"
frameworks="${NBTEVO_FRAMEWORKS_DIR:-$PWD/frameworks}"
runtime="${NBTEVO_RUNTIME_DIR:-$PWD/runtime}"
for lib in libAppFramework.A.so libCoreFramework.A.so; do
    test -f "$frameworks/$lib" || { echo "Missing private framework: $frameworks/$lib" >&2; exit 1; }
done
export PATH="$QNX_HOST/usr/bin:$PATH"
mkdir -p out
for pair in libm.so:libm.so.2 libsocket.so:libsocket.so.3 libstdc++.so:libstdc++.so.6; do
    name=${pair%:*}
    versioned=${pair#*:}
    if test -f "$runtime/$versioned"; then ln -sf "$runtime/$versioned" "out/$name"; fi
done
ntoarm-gcc -dumpversion
ntoarm-gcc -dumpmachine
c() { qcc -V4.4.2,gcc_ntoarmv7le -Wc,-std=c99 -Wall -Wextra -Werror "$@"; }
cxx() { qcc -V4.4.2,gcc_ntoarmv7le_gpp -lang-c++ -Wc,-std=gnu++98 -Wall -Wextra -Werror "$@"; }
# Game Room: the one page for Freedoom, Tyrian and the Quake games (its programs are the three engines').
cxx -Isrc -Isdk -O2 -fno-exceptions -fPIC -shared src/gameroom.cpp     "$frameworks/libAppFramework.A.so" "$frameworks/libCoreFramework.A.so" -Lout     -Wl,--allow-shlib-undefined -Wl,-soname,libGameRoom.so -o out/libGameRoom.so
doom_dir=${DOOM_DIR:-../../third_party/doomgeneric/doomgeneric}
mkdir -p out/doom
for f in dummy am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop d_main d_mode d_net f_finale f_wipe          g_game hu_lib hu_stuff info i_cdmus i_endoom i_joystick i_scale i_sound i_system i_timer memio m_argv          m_bbox m_cheat m_config m_controls m_fixed m_menu m_misc m_random p_ceilng p_doors p_enemy p_floor          p_inter p_lights p_map p_maputl p_mobj p_plats p_pspr p_saveg p_setup p_sight p_spec p_switch          p_telept p_tick p_user r_bsp r_data r_draw r_main r_plane r_segs r_sky r_things sha1 sounds statdump          st_lib st_stuff s_sound tables v_video wi_stuff w_checksum w_file w_main w_wad z_zone w_file_stdc          i_input i_video doomgeneric mus2mid; do
    qcc -V4.4.2,gcc_ntoarmv7le -O2 -w -DNORMALUNIX -DLINUX -D_QNX_SOURCE -DFEATURE_SOUND -Isrc/sdl_stub         -c "$doom_dir/$f.c" -o "out/doom/$f.o"
done
# Sound effects through the car's own audio (src/freedoom_sound.c: audioconn + the libasound front end).
qcc -V4.4.2,gcc_ntoarmv7le -Wc,-std=gnu99 -Wall -Wextra -Werror -Wno-unused-function -O2 -DNORMALUNIX -DLINUX     -D_QNX_SOURCE -DFEATURE_SOUND -Isrc -I"$doom_dir" -c src/freedoom_sound.c -o out/doom/freedoom_sound.o
qcc -V4.4.2,gcc_ntoarmv7le -Wc,-std=gnu99 -Wall -Wextra -Werror -Isrc -I"${FONT_DIR:-../../third_party/font8x8}" -I"$doom_dir" -O2 -c src/freedoom_worker.c     -o out/doom/freedoom_worker.o
# Music: Chocolate Doom's OPL player + MIDI reader (GPL-2.0) and Nuked OPL3 (LGPL-2.1), src/opl/.
for f in i_oplmusic midifile opl3 opl_queue fd_opl; do
    qcc -V4.4.2,gcc_ntoarmv7le -Wc,-std=gnu99 -O2 -w -DNORMALUNIX -DLINUX -D_QNX_SOURCE -DFEATURE_SOUND -Isrc/opl -I"$doom_dir" -c "src/opl/$f.c" -o "out/doom/opl_$f.o"
done
qcc -V4.4.2,gcc_ntoarmv7le out/doom/*.o -lsocket -lm -o out/freedoom-qnx
# Tyrian: the launcher page and the game: OpenTyrian (GPL-2.0, ../../third_party/opentyrian or $TYR_DIR) with
# src/tyrian/ standing in for SDL2, plus our iDrive back-end (src/tyrian_worker.c, src/car_audio.h).
tyr_dir=${TYR_DIR:-../../third_party/opentyrian/src}
mkdir -p out/tyr
for f in "$tyr_dir"/*.c; do
    b=$(basename "$f" .c)
    extra=
    if test "$b" = opentyr; then extra=-Dmain=tyrian_main; fi
    qcc -V4.4.2,gcc_ntoarmv7le -Wc,-std=gnu99 -O2 -w -DTARGET_UNIX -DNDEBUG -DOPENTYRIAN_VERSION='"2.1-idrive"' $extra -D_QNX_SOURCE -Isrc/tyrian -I"$tyr_dir" -c "$f" -o "out/tyr/$b.o"
done
qcc -V4.4.2,gcc_ntoarmv7le -Wc,-std=gnu99 -Wall -Wextra -Werror -O2 -D_QNX_SOURCE -Isrc/tyrian -c src/tyrian/sdl_shim.c -o out/tyr/sdl_shim.o
qcc -V4.4.2,gcc_ntoarmv7le -Wc,-std=gnu99 -Wall -Wextra -Werror -Isrc -O2 -c src/tyrian_worker.c -o out/tyr/tyrian_worker.o
qcc -V4.4.2,gcc_ntoarmv7le out/tyr/*.o -lsocket -lm -o out/tyrian-qnx
# LibreQuake: the launcher page and the game: the Quake engine via quakegeneric (GPL-2.0, ../../third_party/quakegeneric
# or $QK_DIR) with id's sound mixer (src/quake/) and our iDrive back-end (src/quake_worker.c). Signed char as on x86.
qk_dir=${QK_DIR:-../../third_party/quakegeneric/source}
mkdir -p out/qk
for f in cd_null chase cl_demo cl_input cl_main cl_parse cl_tent cmd common console crc cvar d_edge d_fill d_init          d_modech d_part d_polyse d_scan d_sky d_sprite d_surf d_vars d_zpoint draw host_cmd host in_null keys mathlib          menu model net_loop net_main net_none net_vcr nonintel pr_cmds pr_edict pr_exec r_aclip r_alias r_bsp r_draw          r_edge r_efrag r_light r_main r_misc r_part r_sky r_sprite r_surf r_vars sbar screen sv_main sv_move sv_phys          sv_user vid_null view wad world zone; do
    qcc -V4.4.2,gcc_ntoarmv7le -Wc,-std=gnu99 -Wc,-fsigned-char -O2 -w -I"$qk_dir" -c "$qk_dir/$f.c" -o "out/qk/$f.o"
done
qcc -V4.4.2,gcc_ntoarmv7le -Wc,-std=gnu99 -Wc,-fsigned-char -O2 -w -DSys_FloatTime=Sys_FloatTime_null -DSys_Error=Sys_Error_null -DSys_SendKeyEvents=Sys_SendKeyEvents_null -I"$qk_dir" -c "$qk_dir/sys_null.c" -o out/qk/sys_null.o
for f in snd_dma snd_mix snd_mem; do
    qcc -V4.4.2,gcc_ntoarmv7le -Wc,-std=gnu99 -Wc,-fsigned-char -O2 -w -I"$qk_dir" -c "src/quake/$f.c" -o "out/qk/$f.o"
done
qcc -V4.4.2,gcc_ntoarmv7le -Wc,-std=gnu99 -Wall -Wextra -Werror -Isrc -I"${FONT_DIR:-../../third_party/font8x8}" -O2 -c src/quake_worker.c -o out/qk/quake_worker.o
qcc -V4.4.2,gcc_ntoarmv7le out/qk/*.o -lsocket -lm -o out/quake-qnx
# Retro: the launcher page and the NES emulator: FCEUmm (GPL-2.0, ../../third_party/fceumm/src or $NES_DIR) as a
# statically linked libretro core, our front end (src/nes_worker.c) and the public-domain font ($FONT_DIR).
nes_dir=${NES_DIR:-../../third_party/fceumm/src}
font_dir=${FONT_DIR:-../../third_party/font8x8}
nes_common="$nes_dir/drivers/libretro/libretro-common"
mkdir -p out/nes
n=0
for f in "$nes_dir"/boards/*.c "$nes_dir"/input/*.c "$nes_dir/drivers/libretro/libretro.c" "$nes_dir/drivers/libretro/libretro_dipswitch.c" "$nes_dir/cart.c" "$nes_dir/cheat.c" "$nes_dir/crc32.c" "$nes_dir/fceu-endian.c" "$nes_dir/fceu-memory.c" "$nes_dir/fceu.c" "$nes_dir/fds.c" "$nes_dir/fds_apu.c" "$nes_dir/file.c" "$nes_dir/filter.c" "$nes_dir/general.c" "$nes_dir/input.c" "$nes_dir/md5.c" "$nes_dir/nsf.c" "$nes_dir/palette.c" "$nes_dir/ppu.c" "$nes_dir/sound.c" "$nes_dir/state.c" "$nes_dir/video.c" "$nes_dir/vsuni.c" "$nes_dir/ines.c" "$nes_dir/unif.c" "$nes_dir/x6502.c" "$nes_common/compat/compat_posix_string.c" "$nes_common/compat/compat_snprintf.c" "$nes_common/compat/compat_strcasestr.c" "$nes_common/compat/compat_strl.c" "$nes_common/compat/fopen_utf8.c" "$nes_common/encodings/encoding_utf.c" "$nes_common/file/file_path.c" "$nes_common/file/file_path_io.c" "$nes_common/streams/file_stream.c" "$nes_common/streams/file_stream_transforms.c" "$nes_common/string/stdstring.c" "$nes_common/time/rtime.c" "$nes_common/vfs/vfs_implementation.c"; do
    n=$((n + 1))
    qcc -V4.4.2,gcc_ntoarmv7le -Wc,-std=gnu99 -O2 -w -D__LIBRETRO__ -DPATH_MAX=1024 -DFCEU_VERSION_NUMERIC=9900 -DFRONTEND_SUPPORTS_RGB565 -D_QNX_SOURCE -I"$nes_dir/drivers/libretro" -I"$nes_common/include" -I"$nes_dir" -I"$nes_dir/input" -I"$nes_dir/boards" -c "$f" -o "out/nes/core_$n.o"
done
qcc -V4.4.2,gcc_ntoarmv7le -Wc,-std=gnu99 -Wall -Wextra -Werror -Isrc -I"$nes_common/include" -I"$font_dir" -O2 -c src/nes_worker.c -o out/nes/nes_worker.o
qcc -V4.4.2,gcc_ntoarmv7le out/nes/*.o -lsocket -lm -o out/nes-qnx
# Retro (Game Boy / Color): Gambatte (GPL-2.0, ../../third_party/gambatte or $GB_DIR) as a second libretro core
# in its own program, gb-qnx: the same front end built with RETRO_CORE_GB, linked as C++.
gb_dir=${GB_DIR:-../../third_party/gambatte}
gb_src="$gb_dir/libgambatte/src"
gb_common="$gb_dir/libgambatte/libretro-common"
gb_flags="-O2 -w -D__LIBRETRO__ -DHAVE_STDINT_H -DHAVE_INTTYPES_H -DCC_RESAMPLER_NO_HIGHPASS -DVIDEO_RGB565 -D_QNX_SOURCE -I$gb_src -I$gb_dir/libgambatte/include -I$gb_dir/common -I$gb_dir/common/resample -I$gb_dir/libgambatte/libretro -I$gb_common/include"
mkdir -p out/gb
for f in bootloader cpu gambatte initstate interrupter interruptrequester gambatte-memory sound statesaver tima video video_libretro mem/cartridge mem/cartridge_libretro mem/huc3 mem/memptrs mem/rtc sound/channel1 sound/channel2 sound/channel3 sound/channel4 sound/duty_unit sound/envelope_unit sound/length_counter video/ly_counter video/lyc_irq video/next_m0_time video/ppu video/sprite_mapper; do
    qcc -V4.4.2,gcc_ntoarmv7le_gpp -lang-c++ -Wc,-std=gnu++98 $gb_flags -c "$gb_src/$f.cpp" -o "out/gb/gb_$(echo "$f" | tr / _).o"
done
qcc -V4.4.2,gcc_ntoarmv7le_gpp -lang-c++ -Wc,-std=gnu++98 $gb_flags -c "$gb_dir/libgambatte/libretro/libretro.cpp" -o out/gb/gb_libretro.o
for f in "$gb_dir/libgambatte/libretro/gambatte_log.c" "$gb_dir/libgambatte/libretro/blipper.c" "$gb_dir/libgambatte/libretro/cc_resampler.c" "$gb_common/compat/compat_posix_string.c" "$gb_common/compat/compat_snprintf.c" "$gb_common/compat/compat_strcasestr.c" "$gb_common/compat/compat_strl.c" "$gb_common/compat/fopen_utf8.c" "$gb_common/encodings/encoding_utf.c" "$gb_common/file/file_path.c" "$gb_common/file/file_path_io.c" "$gb_common/streams/file_stream.c" "$gb_common/streams/file_stream_transforms.c" "$gb_common/string/stdstring.c" "$gb_common/time/rtime.c" "$gb_common/vfs/vfs_implementation.c"; do
    qcc -V4.4.2,gcc_ntoarmv7le -Wc,-std=gnu99 $gb_flags -c "$f" -o "out/gb/gbc_$(basename "$f" .c).o"
done
qcc -V4.4.2,gcc_ntoarmv7le -Wc,-std=gnu99 -Wall -Wextra -Werror -DRETRO_CORE_GB -Isrc -I"$gb_common/include" -I"$font_dir" -O2 -c src/nes_worker.c -o out/gb/gb_worker.o
qcc -V4.4.2,gcc_ntoarmv7le_gpp -lang-c++ out/gb/*.o -lsocket -lm -o out/gb-qnx
# Retro (PS1): PCSX ReARMed (GPL-2.0, ../../third_party/pcsx_rearmed or $PS1_DIR) with its ARM dynarec and NEON
# GPU, in its own program ps1-qnx (the same front end built with RETRO_CORE_PS1). The BIOS is the owner's (Your games).
rm -rf out/ps1_src
cp -r "${PS1_DIR:-../../third_party/pcsx_rearmed}" out/ps1_src   # patched for gcc 4.4 (tools/ps1_gcc44.sh), never in place
sh tools/ps1_gcc44.sh out/ps1_src
ps1_dir=out/ps1_src
ps1_compat=src/ps1_compat
. ./build_ps1.inc
qcc -V4.4.2,gcc_ntoarmv7le -Wc,-std=gnu99 -Wc,-marm -Wc,-mcpu=cortex-a9 -O2 -c src/ps1_compat/ps1_atomic.c -o out/ps1/ps1_atomic.o
qcc -V4.4.2,gcc_ntoarmv7le -Wc,-std=gnu99 -Wall -Wextra -Werror -DRETRO_CORE_PS1 -Isrc -I"$ps1_dir/deps/libretro-common/include" -I"$font_dir" -O2 -c src/nes_worker.c -o out/ps1/ps1_worker.o
qcc -V4.4.2,gcc_ntoarmv7le out/ps1/*.o -lsocket -lm -o out/ps1-qnx
c -O2 -static fileops.c -o out/gameroom-fileops
# PSP: the Game Room front end for the PPSSPP libretro core (psp-qnx). It loads ppsspp_libretro.so at run time.
qcc -V4.4.2,gcc_ntoarmv7le -Wc,-std=gnu99 -Wc,-marm -Wc,-mfloat-abi=softfp -O2 -Wall -Wextra -Werror -Wno-unused-function \
    -Isrc src/psp_worker.c -lsocket -lm -o out/psp-qnx
echo "Game Room build done: out/libGameRoom.so and out/*-qnx"
