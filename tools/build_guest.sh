#!/bin/sh
# Builds arm32 guest code with the NDK into build/guest/.
#   guest/tests/*_static.c      -> static executables (no guest linker needed)
#   guest/tests/*_dynamic.c     -> dynamic PIE executables (run with --sysroot)
#   guest/tests/*_dynamic.cpp   -> dynamic C++ executables linked with libzbthrow + libc++_shared
#   guest/testlib/zbthrow.cpp   -> build/guest/lib/libzbthrow.so
#   guest/testlib/zbcallprobe.c -> build/guest/lib/libzbcallprobe.so
#   guest/testlib/zbjniprobe.c  -> build/guest/lib/libzbjniprobe.so
#   guest/testlib/zbt7probe.c   -> build/guest/lib/libzbt7probe.so (real ART T7)
#   guest/testlib/zbloadprobe.c -> build/guest/lib/libzbload*.so
#   guest/testlib/zbloadshim.c  -> build/guest/lib/libzbloadshim.so (opens libzbloadhidden.so)
#   guest/zbhost/zbhost.c       -> build/guest/zbhost
#   guest/zbjni/zbjni.c         -> build/guest/lib/libzbjni.so (guest JNIEnv/JavaVM)
#   guest/stubs/gen/*.S         -> build/guest/lib/<lib>.so host-call stub libraries
#   guest/compat/zbcompat.c     -> build/guest/lib/libzbcompat.so
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
if [ -z "${NDK:-}" ]; then
    if [ -d "$HOME/android-ndk-r29" ]; then
        NDK="$HOME/android-ndk-r29"
    elif [ -n "${LOCALAPPDATA:-}" ] && [ -d "$LOCALAPPDATA/Android/Sdk/ndk" ]; then
        NDK=$(ls -d "$LOCALAPPDATA/Android/Sdk/ndk"/* 2>/dev/null | sort -V | tail -n 1)
    elif [ -n "${ANDROID_HOME:-}" ] && [ -d "$ANDROID_HOME/ndk" ]; then
        NDK=$(ls -d "$ANDROID_HOME/ndk"/* 2>/dev/null | sort -V | tail -n 1)
    else
        NDK="$HOME/android-ndk-r29"
    fi
fi
if [ -z "${NDK_HOST:-}" ]; then
    if [ -d "$NDK/toolchains/llvm/prebuilt/windows-x86_64" ]; then
        NDK_HOST="windows-x86_64"
    elif [ -d "$NDK/toolchains/llvm/prebuilt/linux-x86_64" ]; then
        NDK_HOST="linux-x86_64"
    else
        NDK_HOST="linux-arm64"
    fi
fi
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/$NDK_HOST"
CC="$TOOLCHAIN/bin/armv7a-linux-androideabi21-clang"
CXX="$TOOLCHAIN/bin/armv7a-linux-androideabi21-clang++"
OUT="$ROOT/build/guest"

mkdir -p "$OUT/lib"
"$CC" -c -o "$OUT/abi_check.o" "$ROOT/tools/abi_check.c"

for src in "$ROOT"/guest/tests/*_static.c; do
    name=$(basename "$src" .c)
    "$CC" -static -O2 -Wall -o "$OUT/$name" "$src"
done

for src in "$ROOT"/guest/tests/*_dynamic.c; do
    name=$(basename "$src" .c)
    libs=-llog
    [ "$name" = zlib_dynamic ] && libs="$libs -lz"
    "$CC" -O2 -Wall -o "$OUT/$name" "$src" $libs
done

"$CC" -O2 -Wall -I"$ROOT/core/include" -o "$OUT/zbhost" \
    "$ROOT/guest/zbhost/zbhost.c" -ldl

cp "$TOOLCHAIN/sysroot/usr/lib/arm-linux-androideabi/libc++_shared.so" "$OUT/lib/"
"$CXX" -shared -O2 -Wall -nostdlib++ -Wl,-soname,libzbthrow.so -o "$OUT/lib/libzbthrow.so" \
    "$ROOT/guest/testlib/zbthrow.cpp" -lc++_shared
# Base AAPCS probe library for library_runtime_test.
"$CC" -shared -fPIC -O2 -Wall -Wl,-soname,libzbcallprobe.so -o "$OUT/lib/libzbcallprobe.so" \
    "$ROOT/guest/testlib/zbcallprobe.c"
# Guest JNIEnv and JavaVM, preloaded by zbhost; its probe for jni_bridge_test.
"$CC" -shared -fPIC -O2 -Wall -Wextra -Wno-unused-parameter -I"$ROOT/core/include" -Wl,-soname,libzbjni.so \
    -o "$OUT/lib/libzbjni.so" "$ROOT/guest/zbjni/zbjni.c" "$ROOT/guest/zbjni/gen/hostcalls.S"
"$CC" -shared -fPIC -O2 -Wall -I"$ROOT/core/include" -Wl,-soname,libzbjniprobe.so -o "$OUT/lib/libzbjniprobe.so" \
    "$ROOT/guest/testlib/zbjniprobe.c"
"$CC" -shared -fPIC -O2 -Wall -Wextra -Wno-unused-parameter -I"$ROOT/core/include" \
    -Wl,-soname,libzbt7probe.so -o "$OUT/lib/libzbt7probe.so" \
    "$ROOT/guest/testlib/zbt7probe.c" "$ROOT/guest/testlib/zbjniprobe.c"
"$CC" -shared -fPIC -O2 -Wall -Wextra -Wno-unused-parameter -Wl,-soname,libzbloadprobe.so \
    -o "$OUT/lib/libzbloadprobe.so" "$ROOT/guest/testlib/zbloadprobe.c"
"$CC" -shared -fPIC -O2 -Wall -Wextra -Wno-unused-parameter -DZB_LOAD_VERSION=0x00010008 \
    -Wl,-soname,libzbloadbad.so -o "$OUT/lib/libzbloadbad.so" "$ROOT/guest/testlib/zbloadprobe.c"
"$CC" -shared -fPIC -O2 -Wall -Wextra -Wno-unused-parameter -DZB_LOAD_NO_ONLOAD \
    -Wl,-soname,libzbloadnoonload.so -o "$OUT/lib/libzbloadnoonload.so" "$ROOT/guest/testlib/zbloadprobe.c"
"$CC" -shared -fPIC -O2 -Wall -Wextra -Wno-unused-parameter -DZB_LOAD_UNKNOWN_EXPORT \
    -Wl,-soname,libzbloadunknown.so -o "$OUT/lib/libzbloadunknown.so" "$ROOT/guest/testlib/zbloadprobe.c"
# A guest NativeActivity for native_activity_probe_test.
"$CC" -shared -fPIC -O2 -Wall -Wextra -Wno-unused-parameter -Wl,-soname,libzbnativeprobe.so \
    -o "$OUT/lib/libzbnativeprobe.so" "$ROOT/guest/testlib/zbnativeprobe.c"
# A shim whose JNI_OnLoad opens the library that carries the natives, as Unity plugins do.
"$CC" -shared -fPIC -O2 -Wall -Wextra -Wno-unused-parameter -Wl,-soname,libzbloadhidden.so \
    -o "$OUT/lib/libzbloadhidden.so" "$ROOT/guest/testlib/zbloadhidden.c"
"$CC" -shared -fPIC -O2 -Wall -Wextra -Wno-unused-parameter -Wl,-soname,libzbloadshim.so \
    -o "$OUT/lib/libzbloadshim.so" "$ROOT/guest/testlib/zbloadshim.c"
"$CC" -shared -fPIC -O2 -Wall -Wextra -Wl,-soname,libzbloadskip.so \
    -o "$OUT/lib/libzbloadskip.so" "$ROOT/guest/testlib/zbloadskip.c"
for src in "$ROOT"/guest/tests/*_dynamic.cpp; do
    name=$(basename "$src" .cpp)
    "$CXX" -O2 -Wall -nostdlib++ -o "$OUT/$name" "$src" -L"$OUT/lib" -lzbthrow -lc++_shared
done

for asm in "$ROOT"/guest/stubs/gen/*.S; do
    lib=$(basename "$asm" .S)
    # A library's hand-written companion (guest/stubs/<lib>.S / .c) joins the same .so.
    companion=""
    extra_flags=""
    if [ -f "$ROOT/guest/stubs/$lib.S" ]; then companion="$ROOT/guest/stubs/$lib.S"; fi
    if [ -f "$ROOT/guest/stubs/${lib}_cache.c" ]; then
        companion="$companion $ROOT/guest/stubs/${lib}_cache.c"
        extra_flags="-fPIC -O2 -ffreestanding -fno-builtin -I$ROOT/core/include"
    fi
    "$CC" -shared -nostdlib $extra_flags -Wl,-soname,"$lib.so" -o "$OUT/lib/$lib.so" "$asm" $companion
done

# End-to-end callback ALooper probe. -L precedes the NDK sysroot so -landroid resolves to the
# generated arm32 trap stub rather than the platform library.
"$CC" -shared -fPIC -O2 -Wall -Wextra -Wl,-soname,libzblooperprobe.so \
    -o "$OUT/lib/libzblooperprobe.so" "$ROOT/guest/testlib/zblooperprobe.c" \
    -L"$OUT/lib" -landroid

"$CC" -shared -O2 -Wall -Wl,-soname,libzbcompat.so -o "$OUT/lib/libzbcompat.so" "$ROOT/guest/compat/zbcompat.c"

# EGL/window probe for egl_chain_test (Phase 7a Task 9). Built as a shared library so
# LibraryRuntime can dlopen it and call its "main" symbol directly, the same way the
# guest/testlib/*.c probes are driven; it links against the generated EGL/GLESv2 stubs above.
"$CC" -shared -fPIC -O2 -Wall -Wl,-soname,libzbeglprobe.so -o "$OUT/lib/libzbeglprobe.so" \
    "$ROOT/guest/tests/zbeglprobe.c" -L"$OUT/lib" -lEGL -lGLESv2

"$CC" -shared -fPIC -O2 -Wall -Wextra -Wl,-soname,libzbglcacheprobe.so \
    -o "$OUT/lib/libzbglcacheprobe.so" "$ROOT/guest/testlib/zbglcacheprobe.c" -L"$OUT/lib" -lEGL -lGLESv2

# A guest library's exports are what plugins link against at load time. Compare them with the
# platform's real export sets now, not on the device: a name we stop exporting is otherwise only
# visible as `JNI_ERR returned from JNI_OnLoad` in a game. The check reads the built libraries with
# llvm-nm from the same NDK, so put that toolchain on PATH for it rather than relying on the
# caller's environment.
if command -v python >/dev/null 2>&1; then
    PATH="$TOOLCHAIN/bin:$PATH" python "$ROOT/tools/check_platform_exports.py" --libs "$OUT/lib"
fi

echo "guest build ok"
