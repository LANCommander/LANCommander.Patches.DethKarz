#!/bin/sh
# Builds, with MSYS2 mingw32 gcc:
#   build/dinput.dll                                  standalone patch (dinput.dll proxy + test harness)
#   build/dkpatch.asi                                 the same patches as an ASI plugin (for an ASI loader)
#   build/LANCommander.Interposer.Plugin.Dethkarz.dll LANCommander Interposer plugin (same patches)
#   build/dkpatch/glide2x.dll                         OpenGL Glide 2 (src/glide_gl.c), loaded from <game>/dkpatch/
#   build/headless/glide2x.dll                        null Glide for the multiplayer test harness (DK_HEADLESS)
# DK_VERSION=v1.2.3 sets the version the patch logs (default "dev").
set -e
cd "$(dirname "$0")"
export PATH="/c/msys64/mingw32/bin:$PATH"
# DK_LANE=name builds into build/name instead (parallel work)
OUT=build${DK_LANE:+/$DK_LANE}
mkdir -p $OUT
CFLAGS="-m32 -O2 -Wall -Wextra -shared -static-libgcc -s"
CORE="src/dkpatch.c src/net.c src/widescreen.c src/players.c src/gamepad.c src/pad_fe.c src/pad_rumble.c src/iat.c src/saves.c src/opengl.c src/lod.c src/gfxmenu.c src/interp.c src/tickrate.c"
VERSION_FLAG="-DDK_VERSION=\"${DK_VERSION:-dev}\""

# SDL2 (gamepads), linked statically: the patch stays a single DLL
SDL_PREFIX=$(sdl2-config --prefix)
CFLAGS="$CFLAGS -I$SDL_PREFIX/include/SDL2"
SDL_LIBS="$SDL_PREFIX/lib/libSDL2.a -lm -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32 -lsetupapi -lshell32 -ldinput8"

gcc $CFLAGS "$VERSION_FLAG" -o $OUT/dinput.dll \
    src/dinput_proxy.c $CORE src/test.c src/fakeddraw.c src/dinput.def \
    -Wl,--enable-stdcall-fixup -Wl,--kill-at $SDL_LIBS
objdump -p $OUT/dinput.dll | grep -A3 "Ordinal/Name Pointer" | sed -n 3p

gcc $CFLAGS "$VERSION_FLAG" -o $OUT/dkpatch.asi \
    src/asi.c $CORE -Wl,--exclude-all-symbols $SDL_LIBS

gcc $CFLAGS "$VERSION_FLAG" -o $OUT/LANCommander.Interposer.Plugin.Dethkarz.dll \
    src/interposer_plugin.c $CORE \
    -Wl,--kill-at $SDL_LIBS
objdump -p $OUT/LANCommander.Interposer.Plugin.Dethkarz.dll | grep -A3 "Ordinal/Name Pointer" | sed -n 3p
objdump -p $OUT/LANCommander.Interposer.Plugin.Dethkarz.dll | grep "DLL Name"

mkdir -p $OUT/headless
gcc $CFLAGS -o $OUT/headless/glide2x.dll src/nullglide.c src/nullglide.def

mkdir -p $OUT/dkpatch
gcc $CFLAGS -o $OUT/dkpatch/glide2x.dll src/glide_gl.c src/glide_gl.def -lopengl32 -lgdi32 -luser32
