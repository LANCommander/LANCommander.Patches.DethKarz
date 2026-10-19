#!/bin/sh
# Builds, with MSYS2 mingw32 gcc:
#   build/dinput.dll                                  standalone patch (dinput.dll proxy + test harness)
#   build/LANCommander.Interposer.Plugin.Dethkarz.dll LANCommander Interposer plugin (same patches)
#   build/headless/glide2x.dll                        null Glide for the test harness (DK_HEADLESS, tools/mp.py)
set -e
cd "$(dirname "$0")"
export PATH="/c/msys64/mingw32/bin:$PATH"
mkdir -p build
CFLAGS="-m32 -O2 -Wall -Wextra -shared -static-libgcc"
CORE="src/dkpatch.c src/net.c src/widescreen.c src/players.c"

gcc $CFLAGS -o build/dinput.dll \
    src/dinput_proxy.c $CORE src/test.c src/iat.c src/fakeddraw.c src/dinput.def \
    -Wl,--enable-stdcall-fixup -Wl,--kill-at -lwinmm -lgdi32
objdump -p build/dinput.dll | grep -A3 "Ordinal/Name Pointer" | sed -n 3p

gcc $CFLAGS -o build/LANCommander.Interposer.Plugin.Dethkarz.dll \
    src/interposer_plugin.c $CORE \
    -Wl,--kill-at -lwinmm
objdump -p build/LANCommander.Interposer.Plugin.Dethkarz.dll | grep -A3 "Ordinal/Name Pointer" | sed -n 3p
objdump -p build/LANCommander.Interposer.Plugin.Dethkarz.dll | grep "DLL Name"

mkdir -p build/headless
gcc $CFLAGS -o build/headless/glide2x.dll src/nullglide.c src/nullglide.def
