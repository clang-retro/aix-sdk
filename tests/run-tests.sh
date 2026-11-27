#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 clang-retro contributors
#
# Build the examples for 32-bit AIX and run them in aixrun (tools/aixrun.c,
# which runs static 32-bit AIX programs on Linux, macOS and Windows).
#
#   CC=.../bin/clang SYSROOT=... AIXRUN=... tests/run-tests.sh [OUT]
#
# CC is clang from clang-retro/llvm (with xcoff-ld beside it) and SYSROOT
# the sysroot made by build-sysroot.sh. AIXRUN is the aixrun program
# (default: built from tools/aixrun.c with the host's cc into OUT). OUT
# (default ./tests-out) gets the programs and their output.
#
# The tests build for clang's default CPU on AIX 4.3 (the 601), and for
# the 603 and the 604 too. (clang gives -mcpu=pwr3, pwr4 and pwr5 AltiVec,
# which those POWER processors do not have, so they are not tested.)
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
CC=${CC:-clang}
: "${SYSROOT:?set SYSROOT}"
out=${1:-$PWD/tests-out}
mkdir -p "$out"
out=$(cd "$out" && pwd)
if [ -z "${AIXRUN:-}" ]; then
  AIXRUN=$out/aixrun
  ${HOSTCC:-cc} -O2 -Wall -o "$AIXRUN" "$here/tools/aixrun.c" -lm
fi
fail=0

ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }

# name expected-status expected-text program args...
check() {
  name=$1 status=$2 text=$3 prog=$4
  shift 4
  set +e
  (cd "$out" && ABITEST=yes "$AIXRUN" "$prog" "$@") >"$out/$name.log" 2>&1
  got=$?
  set -e
  if [ $got -ne "$status" ]; then
    bad "$name (exit status $got, not $status)"; tail -5 "$out/$name.log"
  elif grep -q FAIL "$out/$name.log"; then
    bad "$name"; grep FAIL "$out/$name.log"
  elif [ -n "$text" ] && ! grep -qF "$text" "$out/$name.log"; then
    bad "$name (no \"$text\")"; tail -5 "$out/$name.log"
  else
    ok "$name"
  fi
}

# The versioned triple picks the 601 and the clang-retro link (xcoff-ld,
# the SDK's crt0.o, libc, unix.exp and libc.exp).
flags="--target=powerpc-ibm-aix4.3 --sysroot=$SYSROOT -fuse-ld=lld -Wall -Werror"
for o in -O0 -O2 -Os; do
  n=aix$o
  if $CC $flags $o "$here/examples/abitest.c" -o "$out/abitest$n"; then
    ok "build abitest$n"
    check "abitest$n" 0 "abitest: destructor ran" "./abitest$n" one "two words"
  else
    bad "build abitest$n"
  fi
done
for m in 603 604; do
  if $CC $flags -mcpu=$m -O2 "$here/examples/abitest.c" -o "$out/abitest-$m"; then
    ok "build abitest-$m"
    check "abitest-$m" 0 "abitest: destructor ran" "./abitest-$m" one "two words"
  else
    bad "build abitest-$m"
  fi
done
if $CC $flags -O2 "$here/examples/hello.c" -o "$out/hello"; then
  ok "build hello"
  check "hello" 0 "Hello, world from ./hello! 42 123456789 3.25" "./hello"
else
  bad "build hello"
fi
# The exit status gets back to the caller.
printf 'int main(void) { return 3; }\n' >"$out/exit3.c"
if $CC $flags -O2 "$out/exit3.c" -o "$out/exit3"; then
  check "exit3" 3 "" "./exit3"
else
  bad "build exit3"
fi
# What AIX's loader runs: a 32-bit XCOFF executable (magic 0x01DF) whose
# entry is the __start descriptor, with a loader section that imports the
# kernel calls from /unix.
readobj=${READOBJ:-$(dirname "$CC")/llvm-readobj}
h=$("$readobj" --file-headers --auxiliary-header --loader-section-header \
  --loader-section-symbols "$out/hello" 2>&1)
if echo "$h" | grep -q 'Magic: 0x1DF' &&
   echo "$h" | grep -q 'Flags: 0x1007' &&
   echo "$h" | grep -q 'Name: __start' &&
   echo "$h" | grep -q 'Name: kwrite' &&
   echo "$h" | grep -q 'ImportFileID: 0x1'; then
  ok "hello is a 32-bit XCOFF program importing from /unix"
else
  bad "hello is not a 32-bit XCOFF program importing from /unix"; echo "$h"
fi
exit $fail
