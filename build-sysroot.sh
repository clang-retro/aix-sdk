#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 clang-retro contributors
#
# Build the AIX sysroot for static programs on 32-bit PowerPC (AIX 4 and
# later): picolibc with its AIX layer and the compiler-rt builtins, for
# the PowerPC 601, which every system running AIX 4 or 5 can run.
#
#   BIN=... LLVM_SRC=... PICOLIBC_SRC=... ./build-sysroot.sh OUT
#
#   BIN           directory with clang, xcoff-ld, llvm-ar and the other
#                 LLVM tools (clang-retro/llvm, with the PowerPC back end)
#   LLVM_SRC      clang-retro/llvm checkout (compiler-rt)
#   PICOLIBC_SRC  clang-retro/picolibc checkout
#   WORK          build directory (default: ./build-aix)
#   MCPU          CPU the libraries are built for (default: 601)
#
# OUT gets the layout clang's AIX driver links against with xcoff-ld (it
# is the default sysroot when it is ../powerpc-ibm-aix from clang's bin
# directory; otherwise pass it with --sysroot):
#
#   usr/include/  picolibc's headers
#   usr/lib/      crt0.o libc.a libm.a libclang_rt.builtins.a
#                 unix.exp  the kernel calls the library imports from /unix
#                 libc.exp  the symbols the program exports (errno)
#
# Everything in it can be redistributed. Nothing from IBM is used.
set -e
out=${1:?usage: build-sysroot.sh OUT}
: "${BIN:?set BIN}" "${LLVM_SRC:?set LLVM_SRC}" "${PICOLIBC_SRC:?set PICOLIBC_SRC}"
WORK=${WORK:-$PWD/build-aix}
mkdir -p "$out/usr/include" "$out/usr/lib" "$WORK"
out=$(cd "$out" && pwd) WORK=$(cd "$WORK" && pwd)
PATH=$BIN:$PATH
export PATH
# No OS version in the triple: meson would read it as the compiler's
# version. The CPU is given instead.
triple=powerpc-ibm-aix flags=-mcpu=${MCPU:-601}

"$BIN/clang" --target=$triple -print-targets | grep -qw ppc32 ||
  { echo "$BIN/clang has no PowerPC back end" >&2; exit 1; }
[ -x "$BIN/xcoff-ld" ] ||
  { echo "$BIN has no xcoff-ld (lld with XCOFF support)" >&2; exit 1; }

# picolibc, with its malloc on top of the AIX layer's sbrk.
pb=$WORK/picolibc pi=$WORK/picolibc-install
rm -rf "$pb" "$pi"
meson setup "$pb" "$PICOLIBC_SRC" \
  --cross-file "$PICOLIBC_SRC/scripts/cross-clang-powerpc-ibm-aix.txt" \
  -Dc_args="$flags" \
  -Dos-aix=true -Dmultilib=false -Dpicocrt=false \
  -Dsemihost=false -Dtests=false -Dposix-console=true -Dsingle-thread=true \
  -Dthread-local-storage=false -Dspecsdir=none \
  --prefix="$pi" >"$WORK/picolibc.log" 2>&1 ||
  { tail -20 "$WORK/picolibc.log"; exit 1; }
ninja -C "$pb" install >>"$WORK/picolibc.log" 2>&1 ||
  { grep -E 'error|FAILED' "$WORK/picolibc.log" | head -20; exit 1; }
cp -R "$pi/include/." "$out/usr/include/"
for f in crt0.o libc.a libm.a unix.exp libc.exp; do
  cp "$pi/lib/$f" "$out/usr/lib/"
done

# compiler-rt builtins: 64-bit division and the like. They are built with
# the sysroot, so the driver finds picolibc's headers, not the host's.
bb=$WORK/builtins
rm -rf "$bb"
cmake -G Ninja -S "$LLVM_SRC/compiler-rt/lib/builtins" -B "$bb" \
  -DCMAKE_SYSTEM_NAME=Generic -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$BIN/clang" -DCMAKE_ASM_COMPILER="$BIN/clang" \
  -DCMAKE_CXX_COMPILER="$BIN/clang++" \
  -DCMAKE_C_COMPILER_TARGET=$triple \
  -DCMAKE_CXX_COMPILER_TARGET=$triple \
  -DCMAKE_ASM_COMPILER_TARGET=$triple \
  -DCMAKE_C_FLAGS="$flags --sysroot=$out" -DCMAKE_ASM_FLAGS="$flags --sysroot=$out" \
  -DCMAKE_AR="$BIN/llvm-ar" -DCMAKE_RANLIB="$BIN/llvm-ranlib" \
  -DCMAKE_NM="$BIN/llvm-nm" \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON -DCOMPILER_RT_BAREMETAL_BUILD=ON \
  -DCOMPILER_RT_OS_DIR=aix -DLLVM_CMAKE_DIR="$BIN/../lib/cmake/llvm" \
  >"$WORK/builtins.log" 2>&1 ||
  { tail -20 "$WORK/builtins.log"; exit 1; }
ninja -C "$bb" >>"$WORK/builtins.log" 2>&1 ||
  { grep -E 'error|FAILED' "$WORK/builtins.log" | head -20; exit 1; }
cp "$bb"/lib/aix/libclang_rt.builtins-*.a "$out/usr/lib/libclang_rt.builtins.a"
echo "sysroot written to $out"
