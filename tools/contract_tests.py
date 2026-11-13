#!/usr/bin/env python3
"""Every contract test of ceesafec, against CeeSafe's own expectations only (Decision 0007):
- the fixed point at HEAD, with ceesafec as stage 0 (tools/fixed_point.py);
- CeeSafe's corpus (tests/reference/tests/corpus) in every family (and, on POSIX, ffi/imported-enums with its enums
  unsigned, `u32`, as before 0.28.0): annotations, AST/HIR/C snapshots, expected output,
  the optimisation reports; run-pass and run-fail again in the interpreter; run-pass, run-fail and ffi again under
  -Werror, with `--split --jobs 4` at -O2, and under ASan+UBSan with leak checks; lib/'s own diagnostics (Decision 0008); the async programs again
  on the executor's poll() fallback (CEESAFE_REACTOR_POLL=1);
- the support library's tests with leak checks, and its allocation-failure sweep;
- the mutation suite, the fault sweep over the corpus, the cancellation sweep over its async programs, the package
  tests (tools/package_tests.py), the native DNS checks (tools/dns_check.py), a file over 4 GiB sent
  (tools/sendfile_check.py), UDP's ECN, segments and cancellation (tools/udp_check.py), the wall clock (tools/clock_check.py), random bytes
  (tools/random_check.py), a dialogue on standard input (tools/console_input_check.py, ADR 0106), a server
  stopped by SIGINT and SIGTERM (tools/signal_check.py, ADR 0125 and 0126), C's allocations (tools/foreign_alloc_check.py), task frame
  stacks (tools/frame_stack_check.py), allocation counting (tools/alloc_counting_check.py), embedding in a C host
  (tools/embed_check.py, ADR 0088), the
  cancellation wake count (tools/wake_check.py), `ceesafec test` on its fixture (tools/test_command_check.py),
  the collection
  properties, the freestanding checks;
- CeeSafe's specification and guide (tools/spec_check.py, tools/guide_check.py), the book (tools/book_check.py,
  Decision 0011), the debugger scripts, and the MCP server's name lookup (tools/mcp_api_check.py).
- `extern "C"` blocks generated from C headers, by `ceesafec cbind` and by its oracle tools/cbind.py
  (tools/cbind_check.py; ADR 0095, ADR 0127).
- Plain statics of scalar data emitted `const` (tools/const_statics_check.py; ADR 0098).
- The command line (tools/cli_check.py; tooling.md §4.4 and §12): usage errors' status, the help texts, which command
  accepts which option, and the allocation variables under `run --interp`.
- `fix` and `fmt` write each file where they read it (tools/fix_paths_check.py; tooling.md §4.4 and §5.3): absolute
  and relative paths, symbolic links and packages, run from another directory.
Each step's last line is printed.

usage: python3 tools/contract_tests.py --ceesafec PATH
"""

import argparse
import glob
import platform
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from fault_injection import SANITIZER_CC  # noqa: E402  clang, or GCC where there is none
ROOT = os.path.normpath(os.path.join(HERE, ".."))

# GCC 12 -O2 -Werror rejects this program's C (-Wuninitialized in an enum constructor; ROADMAP §P5).
KNOWN_GCC_RUN_FAILURES = {"run-pass/nested-patterns.cee"}


def is_gcc():
    """Whether the default C compiler ($CC, else cc) is GCC."""
    cc = os.environ.get("CC") or "cc"
    version = subprocess.run([cc, "--version"], capture_output=True, text=True).stdout
    return "Free Software Foundation" in version or version.startswith("gcc")


def unsigned_enum_corpus(work):
    """A corpus of one program, ffi/imported-enums in the form it had before 0.28.0: its header's enums have no
    negative enumerator, so GCC and Clang on Linux and macOS give them `unsigned int`, and the program declares them
    `u32` (type-system.md §31.6). The corpus's own form is `int` on every target, so without this no run covers an
    unsigned imported enum. POSIX only: MSVC gives every enum `int`."""
    source = os.path.join(ROOT, "tests", "reference", "tests", "corpus", "ffi")
    target = os.path.join(work, "unsigned-enum", "ffi")
    os.makedirs(target)
    edits = {
        "imported-enums.types.h": [("NO_COLOR = -1, ", ""), ("NO_SIZE = -1, ", "")],
        "imported-enums.cee": [
            ("enum color: i32;", "enum color: u32;"),
            ("enum size_kind: i32;", "enum size_kind: u32;"),
            ("=> a as i32 * 10 + s as i32;", "=> truncate<i32>(a as u32 * 10 + s as u32);"),
            ("x.amount + was as i32;", "x.amount + truncate<i32>(was as u32);"),
            ("tally.sum + c as i32;", "tally.sum + truncate<i32>(c as u32);"),
        ],
    }
    for name in ("imported-enums.cee", "imported-enums.c", "imported-enums.h", "imported-enums.types.h"):
        with open(os.path.join(source, name), encoding="utf-8") as f:
            text = f.read()
        for old, new in edits.get(name, []):
            if old not in text:
                raise SystemExit(f"contract_tests: {name} no longer has {old!r}: update unsigned_enum_corpus")
            text = text.replace(old, new)
        with open(os.path.join(target, name), "w", encoding="utf-8") as f:
            f.write(text)
    return os.path.dirname(target)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ceesafec", required=True)
    args = parser.parse_args()
    cee = os.path.abspath(os.path.expanduser(args.ceesafec))
    corpus = os.path.join(ROOT, "tests", "reference", "tests", "corpus")
    support = sorted(glob.glob(os.path.join(ROOT, "src", "support", "*.cee")))
    py = [sys.executable, "-B"]
    run_corpus = py + [f"{HERE}/run_corpus.py", "--compiler", cee]
    run_families = ["--family", "run-pass", "--family", "run-fail", "--family", "ffi"]
    failed = 0
    with tempfile.TemporaryDirectory(prefix="ceesafe-contract-") as work:
        # lib/ is CeeSafe's (Decision 0008): an empty program checked with the library's own diagnostics on.
        empty = os.path.join(work, "empty.cee")
        with open(empty, "w") as f:
            f.write("fn main() {}\n")
        steps = [
            ("fixed point", py + [f"{HERE}/fixed_point.py", "--stage0", cee, "--work", os.path.join(work, "fp"),
                                  "--manifest", os.path.join(work, "fixed-point.json")]),
            ("corpus", run_corpus + ["--corpus", corpus, "--check-leaks"]),
            # An imported C enum whose integer is unsigned (`enum color: u32`), built and run (POSIX).
            ("unsigned enum", run_corpus + ["--corpus", unsigned_enum_corpus(work), "--family", "ffi",
                                            "--cflag=-Werror"]),
            ("run -Werror", run_corpus + ["--corpus", corpus] + run_families + ["--cflag=-Werror"]),
            # Ubuntu's gcc defines _FORTIFY_SOURCE at -O2, so glibc's warn_unused_result and gcc's flow warnings apply
            # to every build there: the generated C and the runtime pass -O2 -Wall -Werror with it (Linux only).
            ("run fortify", run_corpus + ["--corpus", corpus] + run_families +
             ["--cc", "gcc", "--cflag=-O2", "--cflag=-D_FORTIFY_SOURCE=2", "--cflag=-Wall", "--cflag=-Werror"]),
            # Split units (ADR 0082): each program's C files compiled to their own objects, four at once, at -O2.
            ("run split", run_corpus + ["--corpus", corpus] + run_families +
             ["--cflag=-O2", "--build-option=--split", "--build-option=--jobs", "--build-option=4"]),
            ("run interp", run_corpus + ["--corpus", corpus, "--family", "run-pass", "--family", "run-fail",
                                         "--interp", "--check-leaks"]),
            ("library", [cee, "check", "--library-diagnostics", empty]),
            # The executor's poll() fallback (L1 A4b): the async programs with CEESAFE_REACTOR_POLL=1.
            ("reactor poll", run_corpus + ["--corpus", corpus, "--family", "run-pass", "--filter", "async",
                                           "--check-leaks"]),
            # io_uring (ADR 0035, opt-in on Linux): the async programs on it, under the sanitizers.
            ("reactor uring", run_corpus + ["--corpus", corpus, "--family", "run-pass", "--filter", "async",
                                            "--cc", SANITIZER_CC, "--cflag=-O1", "--cflag=-fsanitize=address,undefined",
                                            "--cflag=-fno-sanitize-recover=all", "--check-leaks"]),
            # Work stealing (ADR 0038): the async programs under ThreadSanitizer, which fails a run that races.
            ("run tsan", run_corpus + ["--corpus", corpus, "--family", "run-pass", "--filter", "async",
                                       "--cc", SANITIZER_CC, "--cflag=-O1", "--cflag=-g", "--cflag=-fsanitize=thread"]),
            # Cells and shared dyn boxes (ADR 0084): the Thread.both programs over Mutex, atomics and `dyn … + Share`.
            ("run tsan share", run_corpus + ["--corpus", corpus, "--family", "run-pass", "--filter", "share",
                                             "--cc", SANITIZER_CC, "--cflag=-O1", "--cflag=-g",
                                             "--cflag=-fsanitize=thread"]),
            # Per-thread scratch stacks and FailingAllocator's atomic countdown: threads that each enter frames.
            ("run tsan scratch", run_corpus + ["--corpus", corpus, "--family", "run-pass", "--filter", "thread-scratch",
                                               "--cc", SANITIZER_CC, "--cflag=-O1", "--cflag=-g",
                                               "--cflag=-fsanitize=thread"]),
            ("run asan", run_corpus + ["--corpus", corpus] + run_families +

             ["--cc", SANITIZER_CC, "--cflag=-O1", "--cflag=-Werror", "--cflag=-fsanitize=address,undefined",
              "--cflag=-fno-sanitize-recover=all", "--check-leaks"]),
            ("support", run_corpus + ["--corpus", os.path.join(ROOT, "tests", "support"), "--family", "run-pass",
                                      "--check-leaks"] + [x for f in support for x in ("--with", f)]),
            ("support sweep", py + [f"{HERE}/fault_sweep.py", "--compiler", cee,
                                    os.path.join(ROOT, "tests", "support", "fault", "alloc-sweep.cee")] + support),
            ("hashmap sweep", py + [f"{HERE}/fault_sweep.py", "--compiler", cee,
                                    os.path.join(ROOT, "tests", "support", "fault", "hash-map-sweep.cee")]),
            ("mutations", py + [f"{HERE}/mutation_suite.py", "--compiler", cee]),
            ("fault sweep", py + [f"{HERE}/fault_injection.py", "--compiler", cee, "--corpus", corpus]),
            ("cancel sweep", py + [f"{HERE}/cancel_sweep.py", "--compiler", cee, "--corpus", corpus]),
            ("packages", py + [f"{HERE}/package_tests.py", "--compiler", cee]),
            ("fmt snapshots", py + [f"{HERE}/fmt_snapshots.py", "--compiler", cee]),
            ("dns", py + [f"{HERE}/dns_check.py", "--compiler", cee]),
            ("sendfile", py + [f"{HERE}/sendfile_check.py", "--compiler", cee]),
            ("udp", py + [f"{HERE}/udp_check.py", "--compiler", cee]),
            ("clock", py + [f"{HERE}/clock_check.py", "--compiler", cee]),
            ("random", py + [f"{HERE}/random_check.py", "--compiler", cee]),
            ("console input", py + [f"{HERE}/console_input_check.py", "--compiler", cee]),
            ("signals", py + [f"{HERE}/signal_check.py", "--compiler", cee]),
            ("window", py + [f"{HERE}/window_check.py", "--compiler", cee]),
            ("foreign alloc", py + [f"{HERE}/foreign_alloc_check.py", "--compiler", cee]),
            ("frame stacks", py + [f"{HERE}/frame_stack_check.py", "--compiler", cee]),
            ("alloc counting", py + [f"{HERE}/alloc_counting_check.py", "--compiler", cee]),
            ("embedding", py + [f"{HERE}/embed_check.py", "--compiler", cee]),
            ("wakes", py + [f"{HERE}/wake_check.py", "--compiler", cee]),
            ("tests", py + [f"{HERE}/test_command_check.py", "--compiler", cee]),
            ("cli", py + [f"{HERE}/cli_check.py", "--compiler", cee]),
            ("fix paths", py + [f"{HERE}/fix_paths_check.py", "--compiler", cee]),
            ("collections", py + [f"{HERE}/collection_properties.py", "--compiler", cee, "--sweep"]),
            ("freestanding", py + [f"{HERE}/freestanding_check.py", "--compiler", cee, "--corpus", corpus]),
            ("spec", py + [f"{HERE}/spec_check.py", "--ceesafec", cee]),
            ("guide", py + [f"{HERE}/guide_check.py", "--ceesafec", cee]),
            ("book", py + [f"{HERE}/book_check.py", "--ceesafec", cee]),
            ("debugger", py + [f"{HERE}/debugger_check.py", "--ceesafec", cee]),
            ("mcp api", py + [f"{HERE}/mcp_api_check.py", "--compiler", cee]),
            ("cbind", py + [f"{HERE}/cbind_check.py", "--ceesafec", cee]),
            ("const statics", py + [f"{HERE}/const_statics_check.py", "--ceesafec", cee]),
        ]
        for name, argv in steps:
            if name == "reactor uring" and platform.system() != "Linux":
                print(f"{name:14} skip io_uring is Linux's (ADR 0035)")
                sys.stdout.flush()
                continue
            if name == "unsigned enum" and platform.system() == "Windows":
                print(f"{name:14} skip MSVC gives every enum `int` (type-system.md §31.6)")
                sys.stdout.flush()
                continue
            if name == "run fortify" and platform.system() != "Linux":
                print(f"{name:14} skip _FORTIFY_SOURCE and gcc are checked on Linux (glibc)")
                sys.stdout.flush()
                continue
            if name in ("run tsan", "run tsan share", "run tsan scratch") and platform.system() == "Windows":
                print(f"{name:14} skip ThreadSanitizer is not on Windows' compilers here")
                sys.stdout.flush()
                continue
            env = dict(os.environ, CEESAFE_REACTOR_POLL="1") if name == "reactor poll" else None
            if name == "reactor uring":
                env = dict(os.environ, CEESAFE_REACTOR_URING="1")
            p = subprocess.run(argv, cwd=ROOT, capture_output=True, text=True, env=env)
            lines = [line for line in (p.stdout + p.stderr).splitlines() if line.strip()]
            summary = " | ".join(lines[-3:]) if name in ("corpus", "run -Werror", "run asan", "support",
                                                         "reactor poll", "reactor uring", "run tsan",
                                                         "run tsan share", "run tsan scratch") else (
                lines[-1] if lines else "(no output)")
            ok = p.returncode == 0
            if name == "freestanding" and p.returncode == 2:
                print(f"{name:14} skip {summary}")  # no clang on this host: not a pass
                sys.stdout.flush()
                continue
            failures = {line.split()[1] for line in lines if line.startswith("FAIL ")}
            if name == "run -Werror" and not ok and failures and failures <= KNOWN_GCC_RUN_FAILURES and is_gcc():
                ok = True
                summary += f" (GCC: {', '.join(sorted(failures))}, recorded)"
            print(f"{name:14} {'ok  ' if ok else 'FAIL'} {summary}")
            sys.stdout.flush()
            failed += not ok
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
