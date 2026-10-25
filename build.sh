#!/bin/sh
# Builds, with MSYS2 mingw32 gcc:
#   build/dinput.dll                                            standalone patch (dinput.dll proxy)
#   build/mcm2patch.asi                                         the same patches as an ASI plugin (for an ASI loader)
#   build/LANCommander.Interposer.Plugin.MotocrossMadness2.dll  LANCommander Interposer plugin (same patches)
# MCM_VERSION=v1.2.3 sets the version the patch logs (default "dev").
set -e
cd "$(dirname "$0")"
export PATH="/c/msys64/mingw32/bin:$PATH"
OUT=build
mkdir -p $OUT
CFLAGS="-m32 -O2 -Wall -Wextra -shared -static-libgcc -s"
CORE="src/mcm2patch.c src/iat.c src/paths.c src/compat.c src/registry.c src/saves.c src/profile.c src/device.c src/dgvoodoo.c src/movie.c src/d3d.c src/widescreen.c src/hud.c src/lod.c src/players.c src/gamepad.c src/pad_rumble.c src/testharness.c src/crashlog.c src/framerate.c"
VERSION_FLAG="-DMCM_VERSION=\"${MCM_VERSION:-dev}\""

# SDL2 (gamepads), linked statically: the patch stays a single DLL
SDL_PREFIX=$(sdl2-config --prefix)
CFLAGS="$CFLAGS -I$SDL_PREFIX/include/SDL2"
SDL_LIBS="$SDL_PREFIX/lib/libSDL2.a -lm -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32 -lsetupapi -lshell32 -ldinput8 -lvfw32"

gcc $CFLAGS "$VERSION_FLAG" -o $OUT/dinput.dll \
    src/dinput_proxy.c $CORE src/dinput.def \
    -Wl,--enable-stdcall-fixup -Wl,--kill-at $SDL_LIBS
objdump -p $OUT/dinput.dll | grep -A12 "Ordinal/Name Pointer" | sed -n '2,9p'

gcc $CFLAGS "$VERSION_FLAG" -o $OUT/mcm2patch.asi \
    src/asi.c $CORE -Wl,--exclude-all-symbols $SDL_LIBS

gcc $CFLAGS "$VERSION_FLAG" -o $OUT/LANCommander.Interposer.Plugin.MotocrossMadness2.dll \
    src/interposer_plugin.c $CORE \
    -Wl,--kill-at $SDL_LIBS
objdump -p $OUT/LANCommander.Interposer.Plugin.MotocrossMadness2.dll | grep -A3 "Ordinal/Name Pointer" | sed -n 3p

# the Indeo 5 movie decoder (FFmpeg, LGPL), a separate DLL: build/mcm2patch/ir50.dll
sh build_ir50.sh
