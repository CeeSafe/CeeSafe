# ADR 0248 — Float math on Windows: the system's UCRT, whatever compiles the program

- Status: **Proposed — implemented** 2026-11-25 on the branch `fix/windows-libm` (from 0.95.6), for the SQLite port's
  `CEESAFE_FINDINGS.md` finding 50, at the owner's decision: "the same as the platform". To ship in 0.95.8 with
  `fix/windows-processes`. §6's question is the owner's.
- Scope: type-system.md §35.8 and c-backend.md §1 (ADR 0218's float methods); `runtime/hosted/ceesafe_rt_math.c`
  (the Windows resolver), the embedded runtime (`src/library/runtime.cee`, generated), `src/interpreter/math.cee`
  (a comment: §2.3); `tools/libm_check.py` (new) and `.github/workflows/windows.yml`.
  Amends ADR 0218 §2.2.

## 1. Context

ADR 0218 made `x.exp()`, `x.pow(y)`, the trigonometric and hyperbolic functions, `fma`, `remainder` and the rest
calls of "the platform C library's function". On Linux, macOS and FreeBSD that is one library, the system's libm,
whatever compiles the program. On Windows it is not, and finding 50 met the difference: the SQLite engine built
with gcc (WinLibs MinGW-w64 16.2, UCRT) differs from sqlite.org's `sqlite3.exe` in the last bit or two for some
arguments of `asin`, `acos`, `cosh`, `tanh` and `exp`, and `pow(2, -1075)`, exactly half the smallest subnormal,
is 4.94e-324 instead of 0.0.

**Why, measured** (a Windows 11 22H2 laptop, ucrtbase.dll 10.0.22621.3593, Visual Studio 2022 Build Tools 14.44 with
Windows SDK 10.0.26100, WinLibs gcc 16.2 UCRT; 82,000 results of 19 functions and `pow`, compared bit for bit):

1. **That gcc does not supply these functions.** With `-Wl,--trace-symbol`, every one of ADR 0218's functions but
   `scalbn` (exact, from libmingwex) binds to `libmsvcrt.a`'s import of `api-ms-win-crt-math-l1-1-0.dll`, which is
   ucrtbase.dll's. A UCRT MinGW calls the system's UCRT. Python's `ctypes.CDLL("ucrtbase")` gives what the gcc
   build gives, every bit, and so does a `cl /MD` build.
2. **`cl` links the UCRT statically by default.** `cl` without `/MD` is `/MT`: `libucrt.lib`'s `exp.obj`, `pow.obj` …
   from the Windows SDK are copied into the program (`/link /VERBOSE`). That copy is the SDK's version of the UCRT's
   math, not the system's. On the laptop it differs from ucrtbase.dll in 1,990 of the 82,000 results (exp2 1,584,
   cosh 188, sinh 99, tanh 50, asin 38, acos 21, atan 7, pow 3), the `pow(2, -1075)` tie among them: the SDK's
   rounds it to 0.0, the system's 22621 DLL to 4.94e-324.
3. **sqlite.org's `sqlite3.exe` is such a static build**: it imports nothing but KERNEL32.dll. Its math is the UCRT of
   the SDK it was built with, frozen into the file. ceesafec's own `cl` command (tooling.md §8.3) is static too.
4. Neither the C compiler's flags nor the floating-point state explains it: `cl /O2`, `/O2 /Oi-` and `/MT` give the
   same bits, and setting the x87 control word to MSVC's 53 bits in the gcc build (MinGW's start-up leaves 64) gives
   the same differences. ucrtbase.dll's FMA3 paths are on by default on an FMA3 CPU in both builds;
   `_set_FMA3_enable(0)` makes more differences, not fewer.

An msvcrt-flavour MinGW (some CI images' gcc) is a third library again: msvcrt.dll's older functions, and
libmingwex's own for the C99 ones msvcrt.dll does not have (`exp2`, `log2`, `cbrt`, `expm1`, `log1p`, the inverse
hyperbolic functions …).

So which libm a CeeSafe program on Windows had depended on the C compiler (and, for `cl`, on the SDK installed when
it was built), not on the machine it runs on.

## 2. Decision

1. **On Windows, every float method of ADR 0218 is ucrtbase.dll's function**, the system's C runtime, whatever
   compiles the program: `cl` (static or not), clang for the MSVC target, MinGW-w64 gcc of either flavour, clang
   for MinGW. This is the owner's "the same as the platform": a CeeSafe program computes what the system's own
   dynamically linked programs compute on the same machine (Python's `math`, anything built `/MD`), and two builds
   of one program by different compilers agree bit for bit.
2. **How.** `ceesafe_rt_math.c`, on Windows, resolves the functions once, at the first call of any of them
   (`InitOnceExecuteOnce`): `LoadLibraryExW(L"ucrtbase.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32)` (System32 only, so
   no DLL in the program's directory or the current one can stand in), then `GetProcAddress` for each name, the `f`
   forms included. The library is never unloaded. A call then goes through the function pointer. This is the small
   C adapter for a host service that AGENTS.md allows; user code sees nothing of it and needs no `unsafe`.
   - Where ucrtbase.dll cannot be loaded (Windows 7 or 8.1 without the UCRT update, KB2999226) or lacks a name, that
     function stays the one the program was linked with, as before this ADR. ucrtbase.dll ships with every Windows
     10 and 11 and Server 2016 and later.
   - Mixing C runtimes is safe here: the functions are pure (numbers in, a number out), they touch no CRT state but
     `errno`, which CeeSafe does not read (ADR 0218 §2.2), and they need no initialisation by the program's CRT.
3. **The interpreter, not yet.** `src/interpreter/math.cee` calls libm by name (`extern "C"`, ADR 0218 §2.2), so
   under `run --interp` on Windows the results are those of the C library ceesafec was linked with: ucrtbase.dll's
   when a UCRT MinGW or `cl /MD` built ceesafec, the SDK's static copy when `cl` built it as the seed's commands and
   release.yml do (§5 measures how often that differs). It should call the runtime's `ceesafe_math_*` instead, which
   every ceesafec already links (its embedded runtime names them), and the interpreter would then agree with builds
   bit for bit on Windows too. But the tree must build with stage 0, 0.94.5 (bootstrap/reference.md), whose runtime
   has no `ceesafe_rt_math.c` and whose checker has no float methods, so neither spelling links or checks there.
   The change is one line per function once stage 0 is a release with ADR 0218 (an explicit change of stage 0, the
   owner's: §6).
4. **Elsewhere nothing changes**: Linux, macOS, FreeBSD, Android and iOS call the system libm, as their `sqlite3`
   does. The generated C does not change on any platform: the change is inside the runtime file.
5. **What is not promised.** Results may differ between operating systems, and between versions of Windows
   (ucrtbase.dll is updated with the system): the promise is agreement with the system the program runs on, and
   between compilers on it. A statically linked program of another language, `sqlite3.exe` among them, carries its
   SDK's UCRT and may differ from a CeeSafe program in the last bit or two. No correctly rounded implementation is
   added (ADR 0218 §3).

## 3. Options not taken

- **Leave each compiler its own libm.** The finding: a program's results depend on how it was built.
- **Make every compiler link the SDK's static UCRT**, which would agree with `sqlite3.exe` on machines where the
  SDKs match. MinGW cannot link `libucrt.lib` (and a MinGW machine has no SDK), and results would still depend on
  which SDK was installed when the program was built, not on anything the user's machine has.
- **`__imp_` imports or link order** with MinGW. A UCRT MinGW already binds to ucrtbase.dll (§1.1), so this fixes
  only gcc and only one flavour; `cl /MT` and an msvcrt MinGW would still differ, and an import of ucrtbase.dll from
  an msvcrt build links two CRTs' startup.
- **`/MD` for `cl` builds.** Fixes `cl` alone, and changes every program's deployment (the VC runtime DLL), for math.
- **Correctly rounded functions** (CORE-MATH): the same results on every system, but not the platform's, which the
  owner decided against for now (ADR 0218 §3, §6.2).

## 4. Consequences

- A CeeSafe program built on Windows by `cl` or by gcc prints the same floats, and the same as Python on that
  machine. The SQLite engine built by either agrees with `sqlite3.exe` exactly where the system's ucrtbase.dll and
  the SDK `sqlite3.exe` was built with agree; on the laptop of §1 (Windows 11 22H2) its three math tests still differ
  from that file (§5).
- The first math call of a Windows program loads ucrtbase.dll if the program has not already (a UCRT program has);
  each call is then one indirect call more and one `InitOnce` check.
- Until §2.3 is done, `run --interp` on Windows can differ from a build in the last bit, where the static UCRT of
  the SDK that built ceesafec differs from the system's ucrtbase.dll.

## 5. Tests and evidence

- `tools/libm_check.py --compiler <ceesafec> [--cc <cc> …]` builds one program with every method on the finding's
  arguments (the SQLite tests' sweeps and special values, `pow(2, -1075)` among them) and seeded random operands, in
  `f64` and `f32`, with each C compiler given and under `run --interp`, and compares every result bit for bit with
  each other and with the system's libm called through Python's `ctypes` (ucrtbase.dll on Windows), and the `f64`
  ones with Python's `math` where it calls libm. On Windows the interpreter's differences are reported as
  information (§2.3). With `cl` among the compilers it also reports, as information, how
  a C program built `cl /MT` (the SDK's static UCRT) differs. windows.yml runs it with `cl` and the runner's gcc.
- Results: ROADMAP §10.

## 6. Questions for the owner

1. **sqlite3.exe.** sqlite.org's `sqlite3.exe` is static (§1.3), so "the same as the platform" and "the same as
   sqlite3.exe" are different promises on a Windows whose ucrtbase.dll is older (or newer) than that file's SDK.
   This ADR keeps the first, as decided. The SQLite port's tests can compare with a `sqlite3` built `/MD` (or by
   MinGW), or accept the last bit on Windows; or CeeSafe could promise the second, which only bundling a libm would
   give.
2. **Stage 0 for the interpreter** (§2.3): move stage 0 to a release with ADR 0218 (0.95.x) so the interpreter can
   call the runtime's functions, or leave the interpreter on the C library ceesafec links until stage 0 moves for
   another reason?
