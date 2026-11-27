# aix-sdk

A sysroot, examples and tests for IBM AIX on 32-bit PowerPC, built with
clang and its XCOFF linker, `xcoff-ld`, from
[clang-retro/llvm](https://github.com/clang-retro/llvm). Both run on Linux
and Windows; nothing from IBM is needed to build a program.

| System | Releases | State |
|---|---|---|
| IBM AIX | 4.1 to 5.3 (32-bit) | static programs work, tested in aixrun |
| IBM AIX | 6.1 and later | should run the same programs; not tried |

The C library is [picolibc](https://github.com/clang-retro/picolibc) with
its AIX layer (`libos/aix`). Nothing from IBM is in this repository or in
the sysroot. None of it has run on a real AIX system yet.

## In short

    BIN=.../bin LLVM_SRC=llvm PICOLIBC_SRC=picolibc ./build-sysroot.sh <bin>/../powerpc-ibm-aix
    clang --target=powerpc-ibm-aix4.3 hello.c -o hello
    cc -o aixrun tools/aixrun.c -lm && ./aixrun ./hello

`BIN` is the directory with clang, xcoff-ld and the LLVM tools, `LLVM_SRC`
a checkout of clang-retro/llvm (for compiler-rt) and `PICOLIBC_SRC` one of
clang-retro/picolibc. `<bin>/../powerpc-ibm-aix` is where clang looks for
the sysroot by default; anywhere else, pass it with `--sysroot` and add
`-fuse-ld=lld`.

## What a program is

clang's AIX driver links a 32-bit XCOFF executable with xcoff-ld, the
way IBM documents static linking (`ld -bnso -bI:/lib/syscalls.exp`):

- `.text` at 0x10000000 and `.data` (with the TOC at its end) at
  0x20000000, where AIX's binder puts them. The entry point is the
  `__start` function descriptor.
- AIX has no system call numbers. The kernel exports its calls by name
  from `/unix`, and the system loader binds a program's imports to them
  when it starts. The sysroot's `unix.exp` lists the calls the C library
  uses, and xcoff-ld makes the glue code (glink) that calls them through
  their descriptors.
- The kernel stores the error number of a failed call in the program's
  exported `errno` variable (`libc.exp`); the C library maps AIX's error
  numbers to its own when the program reads `errno`.
- Static constructors and destructors (clang's `__sinit` and `__sterm`
  functions) are collected by xcoff-ld and run by the startup code and
  `exit`.
- The code is for the PowerPC 601, clang's default for AIX 4 and 5, so
  it runs on every machine those releases support. `-mcpu=603` and
  `-mcpu=604` work too. Leave `pwr3`, `pwr4` and `pwr5` alone for now:
  clang turns on AltiVec for them, which those processors lack.

The kernel call names (`kopen`, `kread`, `kwrite`, `klseek`, `kioctl`,
`accessx`, `statx`, `fstatx`, `gettimer`, `_getpid` and the POSIX-named
ones) and the errno mechanism follow what IBM documents and what AIX's
`truss` shows, but have not been checked against a real
`/lib/syscalls.exp` yet. Not done yet: 64-bit programs, and linking
against AIX's shared `libc.a`.

## Running programs on Linux

`tools/aixrun.c` builds with any C compiler (Linux, macOS or Windows) into
an interpreter for 32-bit PowerPC that loads a static AIX program,
applies its loader relocations, binds its `/unix` imports to emulated
kernel calls and runs it.

    aixrun [-v] [-k] PROGRAM [ARGS...]

`-v` prints each kernel call and `-k` lists the calls aixrun knows. Only
the calls the sysroot's C library makes are emulated.

## Tests

    CC=$BIN/clang SYSROOT=sysroot tests/run-tests.sh tests-out

builds `examples/abitest.c` (calls, struct return, 64-bit, 128-bit and
floating point arithmetic, varargs, setjmp, static constructors, malloc,
files, directories, errors, the command line and time) at `-O0`, `-O2`
and `-Os` and for three CPUs, and `examples/hello.c`, and runs them in
aixrun. It also checks the XCOFF headers. The CI does the same with clang
from clang-retro/llvm's `llvm-prebuilt` release.

## License

Copyright (C) clang-retro contributors.

The clang-retro AIX SDK is free software under the GNU Affero General
Public License, version 3 only (`AGPL-3.0-only`); the full text is in
[LICENSE](LICENSE). If you distribute a program built with it, or let
people use a modified version over a network, you must offer the
program's complete source under the same license.

Each contributor keeps the copyright in their own work and contributes
it under the same license; see [CONTRIBUTING.md](CONTRIBUTING.md).

picolibc and compiler-rt, which the build script builds, have their own
licenses (BSD-style, and Apache 2.0 with LLVM exceptions). AIX is IBM's:
nothing of it is in this repository.
