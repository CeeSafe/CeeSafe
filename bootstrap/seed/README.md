# ceesafec @VERSION@: the C seed

This is the C that ceesafec @VERSION@ generates of itself (`ceesafec build --emit c`), with its runtime. It builds
ceesafec with a C11 compiler alone: no ceesafec or other CeeSafe tool is needed (ROADMAP P9; CeeSafe's ADR 0023,
option A). Every prebuilt binary of this release was built from this archive, and each rebuilt itself from source and
emitted these files again, byte for byte.

## Build

With gcc or clang (Linux, macOS, the BSDs):

```sh
cc -std=c11 -O2 -ffp-contract=off -fno-math-errno -I . *.c -o ceesafec -lm -lpthread
```

With MSVC, in a Developer Command Prompt (x64 or ARM64). `/Oi-` works around an MSVC optimiser bug
(`spec/c-backend.md` §1.1), the compiler needs an 8 MiB stack, and `/bigobj` lifts the
object file's section limit, which the 45 MB `main.c` exceeds:

```bat
cl /nologo /std:c11 /bigobj /O2 /Oi- /MP /I . *.c /Fe:ceesafec.exe /link /STACK:8388608
```

## Check

`./ceesafec --version` prints `ceesafec @VERSION@`. The ceesafec you built emits this seed again from the CeeSafe
sources: in a checkout of the tag `v@VERSION@`,

```sh
/path/to/ceesafec build --emit c -o c && diff -r c /path/to/this/directory
```

shows only `README.md`, `COPYING` and `COPYING.RUNTIME`, which the seed adds.

## Licence

The compiler is GPL-2.0-only (`COPYING`). The runtime it puts into every program carries the Classpath exception
(`COPYING.RUNTIME`).
