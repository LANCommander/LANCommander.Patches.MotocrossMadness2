#!/bin/sh
# Builds build/mcm2patch/ir50.dll: the Indeo Video 5 decoder (FFmpeg's, LGPL 2.1+) as a Video for Windows
# codec, loaded by the patch at run time (src/movie.c) and installed for the game's process only.
# See src/ir50/README.md. Needs MSYS2 mingw32 gcc, make, diffutils, pkgconf and the FFmpeg source:
#   third_party/ffmpeg-src/ffmpeg-$FFMPEG_VERSION (https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz)
set -e
cd "$(dirname "$0")"
export PATH="/c/msys64/mingw32/bin:$PATH"
FFMPEG_VERSION=${FFMPEG_VERSION:-7.1.1}
# SHA-256 of ffmpeg-7.1.1.tar.xz (signature checked against the FFmpeg release key, FCF9 86EA ... D676 58D8)
FFMPEG_SHA256=${FFMPEG_SHA256:-733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1}
TOP=$(pwd)
SRC=$TOP/third_party/ffmpeg-src/ffmpeg-$FFMPEG_VERSION
FF=$TOP/third_party/ffmpeg-build
OUT=$TOP/build/mcm2patch

if [ ! -d "$SRC" ]; then
    mkdir -p third_party/dl third_party/ffmpeg-src
    curl -sfL -o third_party/dl/ffmpeg-$FFMPEG_VERSION.tar.xz https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz
    echo "$FFMPEG_SHA256  third_party/dl/ffmpeg-$FFMPEG_VERSION.tar.xz" | sha256sum -c -
    tar -xJf third_party/dl/ffmpeg-$FFMPEG_VERSION.tar.xz -C third_party/ffmpeg-src
fi

if [ ! -f "$FF/lib/libavcodec.a" ]; then
    mkdir -p "$FF/obj"
    (cd "$FF/obj" && "$SRC/configure" --prefix="$FF" \
        --arch=x86 --target-os=mingw32 --cc=gcc --extra-cflags="-m32 -O2" --extra-ldflags=-m32 \
        --disable-everything --enable-decoder=indeo5 \
        --disable-programs --disable-doc --disable-avdevice --disable-avformat --disable-swresample \
        --disable-swscale --disable-postproc --disable-avfilter --disable-network --disable-x86asm \
        --disable-debug --disable-w32threads --disable-pthreads --disable-os2threads \
        --enable-static --disable-shared --disable-autodetect \
        && make -j8 && make install)
fi

mkdir -p "$OUT"
gcc -m32 -O2 -Wall -Wextra -shared -static-libgcc -s -I"$FF/include" -o "$OUT/ir50.dll" \
    src/ir50/ir50.c src/ir50/ir50.def "$FF/lib/libavcodec.a" "$FF/lib/libavutil.a" \
    -Wl,--enable-stdcall-fixup -lbcrypt -luser32 -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic
objdump -p "$OUT/ir50.dll" | grep -A3 "Ordinal/Name Pointer" | sed -n 3p
ls -l "$OUT/ir50.dll"
