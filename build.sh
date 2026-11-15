#!/bin/sh
# Builds, with clang for the MSVC target (lld-link, the static CRT):
#   build/dinput.dll                                            standalone patch (dinput.dll proxy, Interposer embedded)
#   build/mcm2patch.asi                                         the same as an ASI plugin (Interposer embedded)
#   build/LANCommander.Interposer.Plugin.MotocrossMadness2.dll  LANCommander Interposer plugin (same patches, uses that
#                                                               Interposer)
#   build/mcm2patch/ir50.dll                                    the Indeo 5 movie decoder (build_ir50.sh, MinGW)
#
# Needs (from MSYS2's bash, or Git Bash):
# - clang and lld-link (LLVM; MSYS2: pacman -S mingw-w64-clang-x86_64-clang mingw-w64-clang-x86_64-lld)
# - Visual Studio 2022 or later with the C++ x86 tools and a Windows 10/11 SDK (found with vswhere), unless INCLUDE
#   and LIB are set already
# - vcpkg (VCPKG_ROOT): SDL2 for x86-windows-static (vcpkg.json), and the Interposer's own dependencies
# - CMake (on PATH, or the one that comes with Visual Studio)
# - the LANCommander Interposer source (INTERPOSER_DIR, default ../LANCommander.Interposer). Its static library is
#   built here, into $INTERPOSER_DIR/out/mcm2-x86, with only the features in INTERPOSER_FEATURES (below); an
#   existing build is reused. INTERPOSER_PREBUILT=<dir> links the one in <dir> instead (lib/Release/
#   LANCommander.Interposer.Static.lib and vcpkg_installed/ in it) and builds nothing.
# - for ir50.dll: MSYS2 mingw32 gcc, make, diffutils, pkgconf (see build_ir50.sh)
# MCM_VERSION=v1.2.3 sets the version the patch logs (default "dev").
set -e
cd "$(dirname "$0")"
OUT=build
mkdir -p $OUT

# The Interposer features carried by dinput.dll and the ASI (LANCommander.Interposer/Features.cmake; the core,
# Config.yml, the session log and the plugin config functions, is always there). What the patch uses:
#   DIRECTINPUT  the game's DirectInputCreateA / Ex go through it (DirectInput.DeviceFilter etc. keep working)
#   MATCHES      match statistics (src/matchstats.c)
# A feature not built in is off for the whole game when this Interposer is the first one loaded; add here what a
# LANCommander Interposer configuration for the game should still find (e.g. NETWORK RELAY for its network relay).
INTERPOSER_FEATURES=${INTERPOSER_FEATURES:-"DIRECTINPUT MATCHES"}
ALL_FEATURES="REGISTRY FILES FASTDL DIRECTINPUT AUDIO OSVERSION IDENTITY NETWORK RELAY RICH_PRESENCE MATCHES VOICE
    PLUGINS"

PATH="/c/msys64/clang64/bin:/c/Program Files/LLVM/bin:$PATH"
VSWHERE="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
VS=$("$VSWHERE" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
     -property installationPath 2>/dev/null | tr -d '\r') || true

if [ -z "$INCLUDE" ] || [ -z "$LIB" ]; then
    MSVC="$VS/VC/Tools/MSVC/$(tr -d '\r\n' < "$VS/VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt")"
    SDK="C:/Program Files (x86)/Windows Kits/10"
    SDKV=$(ls "$SDK/Include" | grep '^10\.' | sort -V | tail -1)
    export INCLUDE="$MSVC/include;$SDK/Include/$SDKV/ucrt;$SDK/Include/$SDKV/um;$SDK/Include/$SDKV/shared"
    export LIB="$MSVC/lib/x86;$SDK/Lib/$SDKV/ucrt/x86;$SDK/Lib/$SDKV/um/x86"
fi

# ---------------------------------------------------------------- SDL2 (gamepads), linked statically
if [ -n "$VCPKG_ROOT" ]; then
    "$VCPKG_ROOT/vcpkg" install --triplet x86-windows-static --overlay-triplets=triplets \
        --x-install-root=vcpkg_installed --no-print-usage > /dev/null
fi
SDL=vcpkg_installed/x86-windows-static
[ -f $SDL/lib/SDL2-static.lib ] || { echo "SDL2 not found: set VCPKG_ROOT" >&2; exit 1; }

# ---------------------------------------------------------------- the Interposer's static library
INTERPOSER_DIR=${INTERPOSER_DIR:-../LANCommander.Interposer}
if [ -n "$INTERPOSER_PREBUILT" ]; then
    INTERPOSER_OUT=$INTERPOSER_PREBUILT
else
    INTERPOSER_OUT=$INTERPOSER_DIR/out/mcm2-x86
    CMAKE=$(command -v cmake || echo "$VS/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe")
    switches=
    for f in $ALL_FEATURES; do
        case " $(echo $INTERPOSER_FEATURES) " in
            *" $f "*) switches="$switches -DINTERPOSER_FEATURE_$f=ON" ;;
            *) switches="$switches -DINTERPOSER_FEATURE_$f=OFF" ;;
        esac
    done
    stamp=$INTERPOSER_OUT/mcm2-features.txt
    if [ ! -f "$stamp" ] || [ "$(cat "$stamp")" != "$switches" ]; then
        [ -n "$VCPKG_ROOT" ] || { echo "the Interposer needs VCPKG_ROOT to configure" >&2; exit 1; }
        (cd "$INTERPOSER_DIR" && "$CMAKE" --preset windows-x86 -B out/mcm2-x86 -DINTERPOSER_EMBEDDED_ONLY=ON $switches) \
            > $OUT/interposer-configure.log 2>&1 || { tail -30 $OUT/interposer-configure.log >&2; exit 1; }
        echo "$switches" > "$stamp"
    fi
    "$CMAKE" --build "$INTERPOSER_OUT" --config Release --target LANCommander.Interposer.Static \
        > $OUT/interposer-build.log 2>&1 || { tail -30 $OUT/interposer-build.log >&2; exit 1; }
fi
INTERPOSER_DEPS=$INTERPOSER_OUT/vcpkg_installed/x86-windows-static/lib
INTERPOSER_LIBS="-wholearchive:$INTERPOSER_OUT/lib/Release/LANCommander.Interposer.Static.lib
    $INTERPOSER_DEPS/yaml-cpp.lib $INTERPOSER_DEPS/minhook.x32.lib winhttp.lib"
case " $(echo $INTERPOSER_FEATURES) " in
    *" VOICE "*) INTERPOSER_LIBS="$INTERPOSER_LIBS $INTERPOSER_DEPS/enet.lib $INTERPOSER_DEPS/opus.lib" ;;
esac

# ---------------------------------------------------------------- the patch
CFLAGS="--target=i686-pc-windows-msvc -fms-runtime-lib=static -O2 -Wall -Wextra -Wno-unused-parameter
    -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_WARNINGS -I$SDL/include/SDL2 -DMCM_VERSION=\"${MCM_VERSION:-dev}\""
CORE="src/mcm2patch.c src/iat.c src/paths.c src/compat.c src/registry.c src/saves.c src/profile.c src/device.c
    src/dgvoodoo.c src/movie.c src/d3d.c src/widescreen.c src/hud.c src/lod.c src/players.c src/gamepad.c
    src/pad_rumble.c src/testharness.c src/crashlog.c src/framerate.c src/matchstats.c"
LDFLAGS="-dll -opt:ref -opt:icf -base:0x62000000"
SYSLIBS="$SDL/lib/SDL2-static.lib user32.lib gdi32.lib winmm.lib imm32.lib ole32.lib oleaut32.lib version.lib uuid.lib
    advapi32.lib setupapi.lib shell32.lib ws2_32.lib vfw32.lib dxguid.lib"

# build <output> <extra clang flags> <sources...> -- <extra lld-link arguments...>
build() {
    out=$1 flags=$2
    shift 2
    obj=$OUT/obj/$(basename "$out")
    rm -rf "$obj"
    mkdir -p "$obj"
    objs=
    while [ "$1" != -- ]; do
        o=$obj/$(basename "$1" .c).obj
        clang $CFLAGS $flags -c "$1" -o "$o"
        objs="$objs $o"
        shift
    done
    shift
    lld-link -nologo -out:"$out" -map:"${out%.*}.map" $LDFLAGS $objs "$@" $SYSLIBS
    rm -f "${out%.*}.lib" "${out%.*}.exp"
}

EMBED="-DMCM_INTERPOSER_EMBEDDED -DINTERPOSER_STATIC"
build $OUT/dinput.dll "$EMBED" src/dinput_proxy.c $CORE -- -def:src/dinput.def $INTERPOSER_LIBS
build $OUT/mcm2patch.asi "$EMBED" src/asi.c $CORE -- $INTERPOSER_LIBS
build $OUT/LANCommander.Interposer.Plugin.MotocrossMadness2.dll "" src/interposer_plugin.c $CORE -- \
    -export:InterposerPluginInit=_InterposerPluginInit@4
echo "built: $OUT/dinput.dll $OUT/mcm2patch.asi $OUT/LANCommander.Interposer.Plugin.MotocrossMadness2.dll" \
     "(Interposer features: $(echo $INTERPOSER_FEATURES))"

# the Indeo 5 movie decoder (FFmpeg, LGPL), a separate DLL: build/mcm2patch/ir50.dll
sh build_ir50.sh
