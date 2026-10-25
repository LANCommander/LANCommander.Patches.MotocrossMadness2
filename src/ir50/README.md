# ir50.dll: Indeo Video 5 decoder

The game's movies (`UI\INTRO.AVI`, `UI\EVENTS.AVI` behind the main menu, `UI\CREDITS.AVI`) are Indeo Video 5
(`IV50`), a codec Windows has not shipped since XP. `ir50.dll` is a Video for Windows codec (`DriverProc`)
around FFmpeg's `indeo5` decoder. The patch (`src/movie.c`) loads it from `<game>\mcm2patch\ir50.dll` and
installs it for the game's process only with `ICInstall(ICTYPE_VIDEO, 'IV50', DriverProc, NULL,
ICINSTALL_FUNCTION)`. DirectShow's AVI Decompressor and AVIFile then find it like an installed codec. Nothing
is written to the registry or the system.

It decodes `IV50`/`iv50` to uncompressed RGB: 16 bits (555, or 565 as `BI_BITFIELDS`), 24 or 32 bits, bottom-up
or top-down. The YUV 4:1:0 to RGB conversion is BT.601 studio range.

## License

`ir50.dll` contains FFmpeg (libavcodec and libavutil), which is licensed under the
[GNU Lesser General Public License 2.1 or later](https://www.ffmpeg.org/legal.html). It is a separate library
that the MIT-licensed patch loads at run time. `ir50.c` (the driver wrapper) is part of this repository and
under its MIT license. You can replace the DLL with your own build.

FFmpeg source: [ffmpeg-7.1.1.tar.xz](https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz), unmodified. Its
release signature is checked against the FFmpeg release key `FCF9 86EA 15E6 E293 A564 4F10 B432 2F04 D676 58D8`.
`build_ir50.sh` downloads it (if `third_party/ffmpeg-src` is missing) and configures it with:

```
configure --arch=x86 --target-os=mingw32 --cc=gcc --extra-cflags="-m32 -O2" --extra-ldflags=-m32 \
    --disable-everything --enable-decoder=indeo5 \
    --disable-programs --disable-doc --disable-avdevice --disable-avformat --disable-swresample \
    --disable-swscale --disable-postproc --disable-avfilter --disable-network --disable-x86asm \
    --disable-debug --disable-w32threads --disable-pthreads --disable-os2threads \
    --enable-static --disable-shared --disable-autodetect
```

## Building

MSYS2 with `mingw-w64-i686-gcc`, `make`, `diffutils` and `pkgconf`:

```
sh build_ir50.sh
```

Output: `build\mcm2patch\ir50.dll`.
