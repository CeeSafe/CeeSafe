#!/usr/bin/env python3
"""ADR 0088 stage A's contract tests: CeeSafe embedded in a C host through `build --emit lib` and ceesafe/embed.h.

The fixture is tests/embedding: the package `embed` (exports with threads, async, a @callback, panics, scratch frames)
depending on `tally` (one umbrella build of two packages), C hosts (host.c, host.cpp), the executor's aborts reached
directly (aborts.c) and boxes.cee, the executable form of one export. Checked:
- `--emit lib` (also `--split`): one member per C file, uniquely named; the C host links the archive and every export
  runs, the two packages' code among them; with `-dead_strip` (ld64) or `--gc-sections --icf=safe` (ld.lld, where the
  host links ELF; a skip elsewhere), the exports run and the panic hook fires; with `--cflag -flto=thin`, every member
  is bitcode and a ThinLTO host link runs it (skipped where clang cannot);
- member names: `src/a/b.cee` and `src/a_b.cee` (and a unit against a C file) are two members, both linked; C files
  whose names differ only in case fail the build; a failing archiver is named and leaves the old archive;
- the host's headers: `--emit lib` and `--emit obj` write ceesafe/embed.h and the exports header (its
  `--exports-header` name) beside the output, and the C host is compiled against those alone;
- `main`: a package with a CeeSafe `main` gives an archive without the C `main` (a host with its own links it), and
  with `--lib-main` one that runs alone;
- usage errors: `--ar` or `--lib-main` without `--emit lib`, `--ld` with it, and an archiver that is not found
  (status 2);
- no ceesafe_init: 0.27.0's behaviour (the environment ignored, no report, no arguments, the printed panic);
- ceesafe_init's errors: LATE after an allocation when counting is asked for (nothing changed), and 0 when it is not
  (the hook and argv taken); AGAIN, SIZE (larger, smaller, NULL), ALLOCATOR; two threads calling it at once, one 0;
  two threads that call it and then an export at once, with a third calling ceesafe_shutdown, under ThreadSanitizer
  (AGAIN only once the config is published); argv copied (the host's strings overwritten and freed afterwards);
- the generated `main` starts, and counts when asked, after a C constructor allocated through CeeSafe (0.27.0's);
- fail_alloc_at = k for every k of a single-threaded run, against boxes.cee with CEESAFE_FAIL_ALLOC_AT=k: the same
  outcomes; CEESAFE_INIT_ENVIRONMENT with CEESAFE_ALLOC_REPORT=1: the report from ceesafe_shutdown;
- the panic hook: a bounds panic on the caller's thread, a Thread.spawn thread, an Async.runWorkers child and in a
  @callback, and panic("..."): kind, file, line, column (and message), status 134; a hook that returns, and one that
  calls an export: the printed line once, 134; a hook escaped with longjmp: another thread still calls in, the same
  thread's next export call aborts with the line, whose message (built in a frame since overwritten) is intact; two
  threads panicking at once: both records, one exit;
- the three async aborts reach the hook with CEE_PANIC_ASYNC, their message and no file (aborts.c calls the
  runtime's own functions: they are not reachable from safe code, and the executor's allocations are not counted, so
  fail_alloc_at cannot reach them); without a hook, the line an ordinary program prints, `ceesafe panic: <message>`;
  ceesafe_platform_abort is called only from the panic functions;
- two host threads calling an export (a scratch frame, a FailingAllocator box) 200,000 times each, under
  ThreadSanitizer: no report, every result right (skipped where -fsanitize=thread does not build);
- ceesafe/embed.h included by a C++ host;
- every run-pass and ffi corpus program with an @export, built with `--emit lib --lib-main` and linked from the archive
  alone (its generated `main` is a member): the corpus's expected output, at -O2 and under ASan and UBSan.

usage: python3 tools/embed_check.py --compiler out/ceesafec [--cc clang]
"""

import argparse
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
FIXTURE = os.path.join(ROOT, "tests", "embedding")
CORPUS = os.path.join(ROOT, "tests", "reference", "tests", "corpus")
sys.path.insert(0, HERE)
from run_corpus import expectations  # noqa: E402

LINE = re.compile(r"^ceesafe panic: index out of bounds at src/lib\.cee:\d+:\d+$", re.M)
HOOK = re.compile(r"hook (\w+) kind=(\d+) reason=([^=]*) file=(\S+) line=(\d+) column=(\d+) message=(.*)")
BITCODE = (b"BC\xc0\xde", b"\xde\xc0\x17\x0b")  # LLVM bitcode, raw or in Apple's wrapper


class Check:
    def __init__(self):
        self.passed, self.failed, self.skipped = 0, [], []

    def expect(self, name, ok, detail=""):
        if ok:
            self.passed += 1
            print(f"ok   {name}")
        else:
            self.failed.append(name)
            print(f"FAIL {name}: {detail}")
        sys.stdout.flush()

    def skip(self, name, why):
        self.skipped.append(name)
        print(f"skip {name}: {why}")


def clean_env(extra=None):
    env = {k: v for k, v in os.environ.items() if not k.startswith("CEESAFE_")}
    env.update(extra or {})
    return env


def run(argv, cwd=None, env=None, timeout=120):
    try:
        return subprocess.run(argv, cwd=cwd, capture_output=True, text=True, env=env or clean_env(), timeout=timeout)
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(argv, -999, "", "timed out")


def status_of(p):
    """The shell's status: 128 + signal for a killed process (abort() is 134)."""
    return 128 - p.returncode if p.returncode < 0 else p.returncode


def hooks(text):
    return [HOOK.search(line) for line in text.splitlines() if HOOK.search(line)]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--cc", default=os.environ.get("CC") or ("clang" if shutil.which("clang") else "cc"))
    args = parser.parse_args()
    cee = os.path.abspath(args.compiler)
    cc = args.cc
    cxx = "clang++" if shutil.which("clang++") else "c++"
    check = Check()
    if os.name == "nt":
        print("embed_check: skip, the hosts are POSIX (pthreads); stage E brings Windows (ADR 0088 §2.8)")
        return 0
    with tempfile.TemporaryDirectory(prefix="ceesafe-embed-") as work:
        for name in ("embed", "tally"):
            shutil.copytree(os.path.join(FIXTURE, name), os.path.join(work, name))
        package = os.path.join(work, "embed")
        f = run([cee, "fetch", "--offline"], cwd=package)
        if f.returncode != 0:
            print(f"embed_check: fetch failed: {f.stderr.strip()[-300:]}")
            return 1

        def build(*extra, cwd=package):
            return run([cee, "build", "--offline", *extra], cwd=cwd, timeout=600)

        # Headers: --emit c writes ceesafe/embed.h beside ceesafe/rt.h, and ceesafe_exports.h.
        headers = os.path.join(work, "c")
        b = build("--emit", "c", "-o", headers)
        check.expect("--emit c writes ceesafe/embed.h", b.returncode == 0 and
                     os.path.isfile(os.path.join(headers, "ceesafe", "embed.h")), b.stderr[-300:])

        # The archive, and the host's headers beside it (ADR 0088 §2.1).
        out = os.path.join(work, "out")
        lib = os.path.join(out, "libembed.a")
        b = build("--emit", "lib", "-o", lib)
        check.expect("--emit lib writes ceesafe/embed.h and ceesafe_exports.h beside the archive",
                     b.returncode == 0 and sorted(os.listdir(out)) == ["ceesafe", "ceesafe_exports.h", "libembed.a"] and
                     os.listdir(os.path.join(out, "ceesafe")) == ["embed.h"], f"{b.stderr[-300:]}")
        named = os.path.join(work, "named")
        b = build("--emit", "obj", "--exports-header", "embed_api.h", "-o", os.path.join(named, "embed.o"))
        check.expect("--emit obj writes the headers too, the exports header under its --exports-header name",
                     b.returncode == 0 and os.path.isfile(os.path.join(named, "embed_api.h")) and
                     os.path.isfile(os.path.join(named, "ceesafe", "embed.h")) and
                     "embed_sum" in open(os.path.join(named, "embed_api.h"), encoding="utf-8").read(), b.stderr[-300:])
        members = run(["ar", "t", lib]).stdout.split() if b.returncode == 0 else []
        members = [m for m in members if m.endswith(".o")]
        check.expect("--emit lib: one member per C file", b.returncode == 0 and "embed_main.o" in members and
                     "ceesafe_rt_core.o" in members and "ceesafe_rt_thread.o" in members and
                     "ceesafe_rt_async.o" in members and len(set(members)) == len(members),
                     f"status {b.returncode} {b.stderr[-300:]} members {members}")
        split = os.path.join(work, "libembed_split.a")
        b = build("--emit", "lib", "--split", "-o", split)
        split_members = [m for m in run(["ar", "t", split]).stdout.split() if m.endswith(".o")]
        check.expect("--emit lib --split: a member per unit, the two packages' apart",
                     b.returncode == 0 and len([m for m in split_members if m.startswith("embed_")]) >= 3 and
                     len(set(split_members)) == len(split_members), f"{b.stderr[-300:]} {split_members}")

        # Usage errors (tooling.md §4.1).
        u = build("--emit", "obj", "--ar", "ar", "-o", os.path.join(work, "x.o"))
        check.expect("usage: --ar without --emit lib", u.returncode == 2 and "--ar needs --emit lib" in u.stderr,
                     f"status {u.returncode} {u.stderr[:200]}")
        u = build("--emit", "lib", "--ar", "no-such-archiver-0088", "-o", os.path.join(work, "x.a"))
        check.expect("usage: --emit lib without an archiver", u.returncode == 2 and "--ar" in u.stderr.split("\n")[0],
                     f"status {u.returncode} {u.stderr[:200]}")
        u = build("--emit", "lib", "--ld", "ld.lld", "-o", os.path.join(work, "x.a"))
        check.expect("usage: --ld with --emit lib", u.returncode == 2 and "--ld does not apply" in u.stderr,
                     f"status {u.returncode} {u.stderr[:200]}")
        u = build("--emit", "obj", "--lib-main", "-o", os.path.join(work, "x.o"))
        check.expect("usage: --lib-main without --emit lib", u.returncode == 2 and "--lib-main needs" in u.stderr,
                     f"status {u.returncode} {u.stderr[:200]}")

        named_ar = os.path.join(work, "named_ar", "libembed.a")
        u = build("--emit", "lib", "--ar", "ar", "-o", named_ar)
        check.expect("--ar by a bare name, found on PATH", u.returncode == 0 and os.path.isfile(named_ar),
                     f"status {u.returncode} {u.stderr[:200]}")

        # A failing archiver: named, and the archive already there kept.
        kept = os.path.join(work, "kept.a")
        with open(kept, "w") as handle:
            handle.write("an earlier archive\n")
        u = build("--emit", "lib", "--ar", "/usr/bin/false" if os.path.exists("/usr/bin/false") else "false",
                  "-o", kept)
        check.expect("a failing archiver: named, the earlier archive kept, no temporary left",
                     u.returncode == 1 and "the archiver '" in u.stderr and "C compiler" not in u.stderr and
                     open(kept).read() == "an earlier archive\n" and not os.path.exists(kept + ".partial"),
                     f"status {u.returncode} {u.stderr[-300:]}")

        member_names(check, cee, work)
        library_main(check, cee, cc, work)

        host_c = os.path.join(FIXTURE, "host.c")
        include = ["-I" + out]  # the headers --emit lib wrote, alone

        def link(out, archive, *flags, source=host_c, compiler=cc):
            exe = os.path.join(work, out)
            p = run([compiler, *flags, *include, source, archive, "-o", exe, "-lpthread"], timeout=600)
            return exe if p.returncode == 0 else None, p

        host, p = link("host", lib, "-std=c11", "-O2")
        check.expect("a C host links the archive", host is not None, p.stderr[-500:])
        if host is None:
            return report(check)
        hsplit, p = link("host_split", split, "-std=c11")

        r = run([host, "run"])
        want = "sum=6 twice=42 async=42"
        check.expect("every export runs (threads, async, the dependency's code, a @callback)",
                     r.returncode == 0 and want in r.stdout and "bump=" in r.stdout and "callback=25" in r.stdout and
                     "live=0" in r.stdout, r.stdout + r.stderr)
        if hsplit is not None:
            r = run([hsplit, "run"])
            check.expect("the --split archive's exports run", r.returncode == 0 and want in r.stdout, r.stdout + r.stderr)
        else:
            check.expect("the --split archive links", False, p.stderr[-300:])

        # No ceesafe_init: 0.27.0's behaviour, exactly (ADR 0088 §1.3's table).
        r = run([host, "noinit"], env=clean_env({"CEESAFE_ALLOC_REPORT": "1", "CEESAFE_FAIL_ALLOC_AT": "3"}))
        check.expect("no ceesafe_init: 5 boxes, no arguments, no report, the environment ignored",
                     r.returncode == 0 and "boxes=5 args=0 live=0" in r.stdout and "ceesafe alloc" not in r.stderr,
                     r.stdout + r.stderr)
        r = run([host, "noinit-panic"])
        check.expect("no ceesafe_init: the printed panic, 134", status_of(r) == 134 and LINE.search(r.stderr),
                     f"{status_of(r)} {r.stderr}")

        # ceesafe_init's errors.
        r = run([host, "late"])
        check.expect("LATE after an allocation, counting asked for, twice, and fail_alloc_at not applied",
                     "late=2,2 boxes=5" in r.stdout, r.stdout + r.stderr)
        r = run([host, "late-quiet"])
        found = hooks(r.stdout)
        check.expect("after an allocation, no counting asked for: 0, the hook and argv taken",
                     "quiet=0 args=2" in r.stdout and status_of(r) == 134 and len(found) == 1,
                     f"{status_of(r)} {r.stdout} {r.stderr}")
        r = run([host, "misuse"])
        check.expect("SIZE (larger, smaller, NULL), ALLOCATOR, then 0 with the first version's size, then AGAIN",
                     "larger=3 smaller=3 null=3 allocator=4 first=0 again=1" in r.stdout, r.stdout + r.stderr)
        ok = True
        for _ in range(20):
            r = run([host, "race"])
            ok = ok and "race=0,1" in r.stdout
        check.expect("two threads calling ceesafe_init at once: one 0, one AGAIN (20 runs)", ok, r.stdout)
        r = run([host, "argv"])
        check.expect("argv copied: overwritten and freed by the host, Process.argument unchanged",
                     "args=3 codes=112004,97005,122002" in r.stdout, r.stdout + r.stderr)

        # Failure injection through ceesafe_init, against the executable form.
        exe = os.path.join(work, "boxes")
        b = run([cee, "build", os.path.join(FIXTURE, "boxes.cee"), "-o", exe], timeout=600)
        same = b.returncode == 0
        detail = b.stderr[-300:]
        for k in range(0, 7):
            h = run([host, "boxes", str(k)])
            e = run([exe], env=clean_env({"CEESAFE_FAIL_ALLOC_AT": str(k)} if k else {}))
            if h.stdout.strip() != e.stdout.strip() or (k and k <= 5 and f"boxes={k - 1}" not in h.stdout):
                same = False
                detail = f"k={k}: host {h.stdout.strip()!r}, executable {e.stdout.strip()!r}"
        check.expect("fail_alloc_at = k, every k: the executable's outcomes", same, detail)
        ctor = os.path.join(work, "ctor")
        b = run([cee, "build", os.path.join(FIXTURE, "ctor.cee"), os.path.join(FIXTURE, "ctor.c"), "-o", ctor],
                timeout=600)
        r1 = run([ctor]) if b.returncode == 0 else b
        r2 = run([ctor], env=clean_env({"CEESAFE_ALLOC_REPORT": "1"})) if b.returncode == 0 else b
        check.expect("the generated main after a C constructor allocated through CeeSafe: runs, and counts when asked",
                     r1.returncode == 0 and r1.stdout == "ran\n" and r2.returncode == 0 and r2.stdout == "ran\n" and
                     "ceesafe alloc: attempts=1 live=0" in r2.stderr, f"{b.stderr[-300:]} {r1.stderr} {r2.stderr}")
        r = run([host, "report"], env=clean_env({"CEESAFE_ALLOC_REPORT": "1"}))
        check.expect("CEESAFE_INIT_ENVIRONMENT: CEESAFE_ALLOC_REPORT=1 reported by ceesafe_shutdown",
                     "ceesafe alloc: attempts=6 live=1" in r.stderr and "live=1" in r.stdout, r.stdout + r.stderr)

        # The panic hook (ADR 0088 §2.4).
        for where, kind, message in (("panic", 5, None), ("thread", 5, None), ("worker", 5, None),
                                     ("callback", 2, None), ("explicit", 16, "embed: deliberate")):
            r = run([host, "hook", where])
            found = hooks(r.stdout)
            good = (status_of(r) == 134 and len(found) == 1 and int(found[0].group(2)) == kind and
                    found[0].group(4) == "src/lib.cee" and int(found[0].group(5)) > 0 and int(found[0].group(6)) > 0
                    and (message is None or found[0].group(7) == message) and "ceesafe panic" not in r.stderr)
            check.expect(f"the hook: {where}", good, f"status {status_of(r)} {r.stdout} {r.stderr}")
        r = run([host, "hook-returns"])
        check.expect("a hook that returns: the printed line once, 134",
                     status_of(r) == 134 and len(hooks(r.stdout)) == 1 and len(LINE.findall(r.stderr)) == 1,
                     f"{status_of(r)} {r.stdout} {r.stderr}")
        r = run([host, "hook-reenters"])
        check.expect("a hook that calls an export: the printed line once, 134, the export not run",
                     status_of(r) == 134 and len(LINE.findall(r.stderr)) == 1 and "sum=" not in r.stdout,
                     f"{status_of(r)} {r.stdout} {r.stderr}")
        r = run([host, "escape"])
        check.expect("an escaped hook: other threads call in, the panicked thread's next call aborts",
                     status_of(r) == 134 and "other thread sum=6" in r.stdout and "same thread" not in r.stdout and
                     len(LINE.findall(r.stderr)) == 1, f"{status_of(r)} {r.stdout} {r.stderr}")
        r = run([host, "escape-built"])
        built = "".join(chr(65 + (1 + i) % 26) for i in range(64))
        check.expect("the re-entry line keeps a message built in a frame since overwritten",
                     status_of(r) == 134 and "same thread" not in r.stdout and
                     re.search(rf"^ceesafe panic: {built} at src/lib\.cee:\d+:\d+$", r.stderr, re.M) is not None,
                     f"{status_of(r)} {r.stdout} {r.stderr}")
        r = run([host, "two-panics"])
        records = [line for line in r.stdout.splitlines() if line.startswith("record kind=5 file=src/lib.cee")]
        check.expect("two threads panicking at once: both reports whole, one exit", status_of(r) == 134 and
                     len(records) == 2, f"{status_of(r)} {r.stdout} {r.stderr}")

        # The executor's three aborts (aborts.c compiles the async runtime into itself).
        aborts = os.path.join(work, "aborts")
        runtime = [os.path.join(headers, n) for n in ("ceesafe_rt_core.c", "ceesafe_rt_hosted.c", "ceesafe_rt_io.c",
                                                      "ceesafe_rt_thread.c")]
        p = run([cc, "-std=c11", "-I" + headers, os.path.join(FIXTURE, "aborts.c"), *runtime, "-o", aborts, "-lpthread"],
                timeout=600)
        if p.returncode != 0:
            check.expect("aborts.c builds", False, p.stderr[-500:])
        else:
            for what, message in (("oom", "out of memory for the executor's own bookkeeping"),
                                  ("stuck", "every task is parked with nothing to wake it"),
                                  ("fd-wake", "a task waits on a descriptor and a wake at once")):
                r = run([aborts, what])
                check.expect(f"async abort routed to the hook: {what}", status_of(r) == 134 and
                             f"hook kind=14 file=(null) line=0 message={message}" in r.stdout, r.stdout + r.stderr)
            r = run([aborts, "stuck-print"])
            check.expect("an async abort without a hook: an ordinary program's line, 'ceesafe panic: <message>'",
                         status_of(r) == 134 and
                         r.stderr == "ceesafe panic: every task is parked with nothing to wake it\n",
                         f"{status_of(r)} {r.stderr!r}")
        calls = []
        for directory, _, names in os.walk(headers):
            for name in names:
                if name.endswith(".c"):
                    text = open(os.path.join(directory, name), encoding="utf-8").read()
                    function = None
                    for line in text.splitlines():
                        m = re.match(r"^\S.*?\b(\w+)\([^;]*\)\s*\{\s*$", line)
                        if m:
                            function = m.group(1)
                        if "ceesafe_platform_abort();" in line:
                            calls.append((name, function))
        check.expect("ceesafe_platform_abort is called only from the panic functions",
                     sorted(set(calls)) == [("ceesafe_rt_core.c", "ceesafe_panic_reentered"),
                                            ("ceesafe_rt_core.c", "panic_report")], str(calls))

        # Bun's link flags.
        if platform.system() == "Darwin":
            exe, p = link("host_dead_strip", lib, "-std=c11", "-Wl,-dead_strip")
            if exe is None:
                check.expect("-dead_strip links", False, p.stderr[-300:])
            else:
                r1, r2 = run([exe, "run"]), run([exe, "hook", "callback"])
                check.expect("-dead_strip: the exports run and the hook fires", want in r1.stdout and
                             status_of(r2) == 134 and len(hooks(r2.stdout)) == 1, r1.stdout + r2.stdout)
        if platform.system() == "Darwin":
            check.skip("--gc-sections --icf=safe (ld.lld)", "the host links Mach-O here; ELF hosts run it")
        elif shutil.which("ld.lld") or shutil.which("lld"):
            exe, p = link("host_gc", lib, "-std=c11", "-fuse-ld=lld", "-Wl,--gc-sections", "-Wl,--icf=safe")
            if exe is None:
                check.expect("--gc-sections --icf=safe links", False, p.stderr[-300:])
            else:
                r1, r2 = run([exe, "run"]), run([exe, "hook", "callback"])
                check.expect("--gc-sections --icf=safe: the exports run and the hook fires", want in r1.stdout and
                             status_of(r2) == 134 and len(hooks(r2.stdout)) == 1, r1.stdout + r2.stdout)
        else:
            check.skip("Bun's link flags", "neither ld64 nor ld.lld here")

        # ThinLTO: bitcode members, a ThinLTO host link.
        lto = os.path.join(work, "libembed_lto.a")
        b = build("--emit", "lib", "--cc", cc, "--cflag", "-flto=thin", "-o", lto)
        if b.returncode != 0:
            check.skip("--emit lib with ThinLTO", f"{cc} -flto=thin: {b.stderr.strip()[-200:]}")
        else:
            extracted = os.path.join(work, "lto_members")
            os.makedirs(extracted)
            run(["ar", "x", lto], cwd=extracted)
            names = [n for n in os.listdir(extracted) if n.endswith(".o")]
            bitcode = all(open(os.path.join(extracted, n), "rb").read(4) in BITCODE for n in names)
            check.expect("--cflag -flto=thin: every member is bitcode", bitcode and len(names) >= 5, str(names))
            flags = ["-flto=thin"] + (["-Wl,-dead_strip"] if platform.system() == "Darwin" else
                                      ["-fuse-ld=lld", "-Wl,--gc-sections"] if shutil.which("ld.lld") else [])
            exe, p = link("host_lto", lto, "-std=c11", "-O2", *flags)
            if exe is None:
                check.skip("a ThinLTO host link", p.stderr.strip()[-200:])
            else:
                r1, r2 = run([exe, "run"]), run([exe, "hook", "panic"])
                check.expect("a ThinLTO host link runs the exports and fires the hook", want in r1.stdout and
                             status_of(r2) == 134 and len(hooks(r2.stdout)) == 1, r1.stdout + r1.stderr + r2.stdout)

        # Two host threads, under ThreadSanitizer.
        tsan = os.path.join(work, "libembed_tsan.a")
        b = build("--emit", "lib", "--cc", cc, "--cflag", "-fsanitize=thread", "--cflag", "-g", "--cflag", "-O1",
                  "-o", tsan)
        exe, p = (None, b) if b.returncode != 0 else link("host_tsan", tsan, "-std=c11", "-g", "-O1",
                                                            "-fsanitize=thread")
        if exe is None:
            check.skip("two host threads under ThreadSanitizer", p.stderr.strip()[-200:])
        else:
            r = run([exe, "threads"], env=clean_env({"TSAN_OPTIONS": "halt_on_error=1"}), timeout=900)
            check.expect("two host threads, 200,000 calls each, under ThreadSanitizer: no report, results right",
                         r.returncode == 0 and "seed=1000 wrong=0" in r.stdout and "seed=5000 wrong=0" in r.stdout and
                         "ThreadSanitizer" not in r.stderr, r.stdout + r.stderr[-800:])
        if exe is not None:
            ok, detail = True, ""
            for what in ("init-race", "init-shutdown"):
                for _ in range(30):
                    r = run([exe, what], env=clean_env({"TSAN_OPTIONS": "halt_on_error=1"}))
                    if r.returncode != 0 or "init sums=6000,6001 live=0" not in r.stdout or \
                            "ThreadSanitizer" in r.stderr:
                        ok, detail = False, f"{what}: {r.stdout} {r.stderr[-800:]}"
                        break
            check.expect("ceesafe_init from two threads that then call an export, and ceesafe_shutdown meanwhile, "
                         "under ThreadSanitizer (30 runs each): no report", ok, detail)
        r = run([host, "threads"], timeout=900)
        check.expect("two host threads at -O2: results right", "seed=1000 wrong=0" in r.stdout and
                     "seed=5000 wrong=0" in r.stdout, r.stdout + r.stderr)

        # C++.
        exe, p = link("host_cxx", lib, "-std=c++17", source=os.path.join(FIXTURE, "host.cpp"), compiler=cxx)
        if exe is None:
            check.expect("ceesafe/embed.h in a C++ host", False, p.stderr[-500:])
        else:
            r = run([exe])
            check.expect("ceesafe/embed.h in a C++ host", "c++ sum=6 live=0" in r.stdout, r.stdout + r.stderr)

        corpus_programs(check, cee, cc, work)
    return report(check)


def member_names(check, cee, work):
    """Archive member names (ADR 0088 §2.1): distinct paths give distinct members, so no object replaces another."""
    package = os.path.join(work, "layout")
    files = {"ceesafe.toml": '[package]\nname = "layout"\nkind = "object"\n',
             "src/a/b.cee": '/// One.\n@export("layout_one")\nfn one() -> u64 => 1;\n',
             "src/a_b.cee": '/// Two.\n@export("layout_two")\nfn two() -> u64 => 2;\n',
             "src/a_/b.cee": '/// Four.\n@export("layout_four")\nfn four() -> u64 => 4;\n',
             "src/a/_b.cee": '/// Eight.\n@export("layout_eight")\nfn eight() -> u64 => 8;\n',
             "host.c": '#include <stdio.h>\n#include "ceesafe_exports.h"\nint main(void) {\n'
                       '    printf("%llu\\n", (unsigned long long)(layout_one() + layout_two() + layout_four() +'
                       ' layout_eight()));\n    return 0;\n}\n'}
    for name, text in files.items():
        os.makedirs(os.path.dirname(os.path.join(package, name)), exist_ok=True)
        with open(os.path.join(package, name), "w", encoding="utf-8") as handle:
            handle.write(text)
    archive = os.path.join(package, "out", "liblayout.a")
    b = run([cee, "build", "--offline", "--emit", "lib", "--split", "-o", archive], cwd=package, timeout=600)
    members = [m for m in run(["ar", "t", archive]).stdout.split() if m.endswith(".o")] if b.returncode == 0 else []
    exe = os.path.join(package, "host")
    p = run(["cc", "-I" + os.path.dirname(archive), os.path.join(package, "host.c"), archive, "-o", exe])
    r = run([exe]) if p.returncode == 0 else p
    check.expect("--split: src/a/b, src/a_b, src/a_/b and src/a/_b are four members, all linked",
                 b.returncode == 0 and len(set(members)) == len(members) and r.stdout.strip() == "15",
                 f"{b.stderr[-300:]} {members} {p.stderr[-300:]} {r.stdout}")
    # A C file against a unit: c/layout/x.c (the unit of src/x.cee... here the C file layout_x.c) stay apart too.
    single = os.path.join(work, "single")
    os.makedirs(single)
    for name, text in (("x.cee", '/// X.\n@export("single_x")\nfn x() -> u64 => 1;\n'),
                       ("Twin.c", "int single_upper(void) { return 1; }\n"),
                       ("twin.c", "int single_lower(void) { return 2; }\n")):
        with open(os.path.join(single, name), "w", encoding="utf-8") as handle:
            handle.write(text)
    b = run([cee, "build", "--emit", "lib", "x.cee", "Twin.c", "twin.c", "-o", "out/x.a"], cwd=single, timeout=600)
    check.expect("C files whose member names differ only in case: the build fails, naming the member",
                 b.returncode == 1 and "would share the archive member" in b.stderr and
                 not os.path.exists(os.path.join(single, "out", "x.a")), f"status {b.returncode} {b.stderr[-300:]}")


def library_main(check, cee, cc, work):
    """A package with a CeeSafe `main`: --emit lib leaves the C `main` out unless --lib-main (ADR 0088 §2.1)."""
    source = os.path.join(FIXTURE, "withmain.cee")
    plain = os.path.join(work, "withmain", "libwithmain.a")
    b = run([cee, "build", "--emit", "lib", source, "-o", plain], timeout=600)
    exe = os.path.join(work, "withmain", "host")
    p = run([cc, os.path.join(FIXTURE, "withmain.c"), plain, "-o", exe]) if b.returncode == 0 else b
    r = run([exe]) if p.returncode == 0 else p
    check.expect("--emit lib of a package with a CeeSafe main: no C main, so a host with its own links it",
                 r.returncode == 0 and r.stdout == "host main 42\n", f"{b.stderr[-300:]} {p.stderr[-300:]} {r.stdout}")
    kept = os.path.join(work, "withmain", "libkept.a")
    b = run([cee, "build", "--emit", "lib", "--lib-main", source, "-o", kept], timeout=600)
    alone = os.path.join(work, "withmain", "alone")
    p = run([cc, kept, "-o", alone, "-lpthread"]) if b.returncode == 0 else b
    r = run([alone]) if p.returncode == 0 else p
    check.expect("--lib-main keeps it: the archive linked alone runs the CeeSafe main",
                 r.returncode == 0 and r.stdout == "cee main 5\n", f"{b.stderr[-300:]} {p.stderr[-300:]} {r.stdout}")


def corpus_programs(check, cee, cc, work):
    """Every run-pass and ffi program with an @export, as an archive linked alone: its `main` is a member."""
    programs = []
    for family in ("run-pass", "ffi"):
        directory = os.path.join(CORPUS, family)
        for name in sorted(os.listdir(directory)):
            if not name.endswith(".cee"):
                continue
            text = open(os.path.join(directory, name), encoding="utf-8").read()
            if "@export(" in text and "// option:" not in text and "// profile:" not in text and \
                    "fn main(" in text:
                programs.append((family, name, text))
    for form, flags, link_flags in (("-O2", [], []),
                                    ("ASan+UBSan", ["--cflag", "-O1", "--cflag", "-fsanitize=address,undefined",
                                                    "--cflag", "-fno-sanitize-recover=all"],
                                     ["-fsanitize=address,undefined"])):
        failures = []
        for family, name, text in programs:
            path = os.path.join(CORPUS, family, name)
            inputs = [path]
            if family == "ffi":
                inputs += [path[:-4] + ".c", "--cflag", "-I" + os.path.dirname(path)]
            stem = name[:-4]
            archive = os.path.join(work, f"corpus_{stem}.a")
            b = run([cee, "build", "--emit", "lib", "--lib-main", "--cc", cc, *flags, *inputs, "-o", archive], cwd=ROOT,
                    timeout=600)
            if b.returncode != 0:
                failures.append(f"{family}/{name}: build {b.stderr.strip()[-200:]}")
                continue
            exe = os.path.join(work, f"corpus_{stem}")
            p = run([cc, *link_flags, archive, "-o", exe, "-lpthread", "-lm"], timeout=600)
            if p.returncode != 0:
                failures.append(f"{family}/{name}: link {p.stderr.strip()[-200:]}")
                continue
            r = run([exe], cwd=ROOT)
            stdout, exit_code, panic, _ = expectations(text)
            if r.stdout.replace("\r\n", "\n") != stdout or status_of(r) != exit_code:
                failures.append(f"{family}/{name}: status {status_of(r)}, stdout {r.stdout[-200:]!r}")
        if form == "ASan+UBSan" and failures and all("link" in f or "build" in f for f in failures) and \
                len(failures) == len(programs):
            check.skip(f"corpus programs with @export as archives, {form}", failures[0])
            continue
        check.expect(f"{len(programs)} corpus programs with @export as archives, {form}", not failures and programs,
                     "; ".join(failures[:5]))


def report(check):
    print(f"embed_check: {check.passed} passed, {len(check.failed)} failed, {len(check.skipped)} skipped")
    return 1 if check.failed else 0


if __name__ == "__main__":
    sys.exit(main())
