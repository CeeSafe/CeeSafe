#!/usr/bin/env python3
"""`ceesafec cbind` and tools/cbind.py over their fixture (ADR 0095 §5, ADR 0127): tests/cbind/fixture.h has one of
each row of ADR 0095's table.

The script is the oracle: it is run for every case, the expectations below are checked on what it writes, and the
command is run with the same arguments and must write the same bytes (apart from the name of the generator in the
first line), and succeed or fail as the script does.

1. One configuration: every declaration the ADR's table promises is in the output, and what CeeSafe cannot spell is
   named in the output's comments and declared nowhere. The expectations below are written from the ADR and the
   header, not taken from the tool's output.
2. A program over the binding builds and runs: ceesafec accepts every declaration, and the C compiler, which checks
   each one against the header (type-system.md §31), agrees with all of them.
3. Two configurations that differ in one struct and one macro: the two are listed, the macro is not declared, the
   struct only by name, and the output builds in both configurations.
4. Two headers at once: a type is declared by the header that defines it, once.
5. --only, --leave and --with narrow the output.
6. The script's list of keywords is the lexer's (the command uses the lexer's table).
7. The two write the same for the book's header (book/projects/ch20/clock), with --no-fmt, and for other targets.
8. The command's own behaviour: --help, usage errors (exit status 2), a header that is not found, a C compiler that
   is not clang or cannot be run, $CC, and the compiler it finds when none is named.

usage: python3 tools/cbind_check.py --ceesafec out/ceesafec
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
FIXTURE = HERE.parent / "tests" / "cbind"
EXIT = 47       # tests/cbind/use.cee's sum

# ADR 0095 §2.2, row by row, for tests/cbind/fixture.h (each a line of the formatted output, without its indent).
DECLARED = [
    "fn fx_add(a: i32, b: i32) -> i32;",                                   # a function
    "fn fx_twice(v: u32) -> u32;",                                         # static inline
    "fn fx_log(fmt: *CChar, ...) -> i32;",                                 # variadic
    "struct fx_point {", "x: i32;", "y: i32;",                             # a struct, every member, in order
    '@ctype("fx_value_t")', "struct fx_value_t {", "v: u64;",              # a typedef of a struct with no tag
    "struct fx_box {", "origin: fx_point;", "corners: [fx_point; 4];", "color: fx_color;",
    'notify: extern "C" fn(*mut void) -> i32;',                            # a function pointer member, by its typedef
    'compare: extern "C" fn(*void, *void) -> i32;',                        # and spelled out
    "table: *fx_callback;",                                                # a table of them
    "refs: AtomicU32;", "value: fx_value_t;", "next: *mut fx_node;", "other: *mut other_thing;", "name: *CChar;",
    "label: [u8; 16];", '@cname("type")', "type_: u8;", "live: bool;",     # a keyword member
    "struct fx_bits;", "struct fx_anon;", "struct fx_flex;", "struct fx_vol;",   # opaque, each with its reason
    "// opaque for the bit-field `a`", "// opaque for an anonymous struct or union member",
    '@ctype("fx_either_t")', "struct fx_either_t;",                        # a union's typedef: opaque
    "enum fx_color: u32;", "const FX_RED: fx_color = 0;", "const FX_GREEN: fx_color = 5;", "const FX_BLUE: fx_color = 6;",
    "enum fx_signed: i32;", "const FX_MINUS: fx_signed = -1;",             # a negative value makes it int
    '@ctype("fx_kind_t")', "enum fx_kind_t: u32;", "const FX_T_TWO: fx_kind_t = 2;",
    "const FX_ANON_A: i32 = 3;", "const FX_ANON_B: i32 = 4;",              # an enum with no name: its values
    '@ctype("fx_callback")', "struct fx_callback;",                        # a typedef of a function pointer
    "const FX_LIMIT: i32 = 22;", "const FX_MASK: u32 = 2147483648;", "const FX_NEGATIVE: i32 = -5;",
    "const FX_NAME: *CChar;",                                              # a string literal
    "const FX_WIDTH: i32 = 32;",
    "static mut fx_count: i32;", "static fx_origin: fx_point;", "static fx_names: [*CChar; 4];",
    "static mut fx_current: *CChar;",                                      # a pointer to const is not a const pointer
    "fn type_(in_: i32) -> i32;",                                          # a keyword name, and a keyword parameter
    'fn fx_pick(box: *mut fx_box, cb: extern "C" fn(*mut void) -> i32, argv: **CChar) -> fx_color;',
    'fn fx_visit(n: *mut fx_node, cb: extern "C" fn(*mut void) -> i32, mode: other_mode);',
    "struct fx_node;", "struct other_thing;", "enum other_mode: u32;",     # types of elsewhere: opaque
]
# Names that must be in no declaration, each with the words of its comment.
LEFT_OUT = [
    ("FX_CALL", None),                              # a function-like macro: silently
    ("CBIND_FIXTURE_H", None),                      # an include guard
    ("FX_STATEMENT", "not an expression"),
    ("FX_FLOAT", "not an integer constant"),
    ("fx_make", "result CeeSafe cannot take"),      # a struct by value
    ("fx_vprint", "parameter CeeSafe cannot take"),
    ("fx_tagged", "a union with a tag"),
    ("other_get", None),                            # another header's function
]

problems = []


def problem(text: str):
    problems.append(text)
    print(f"cbind_check: FAIL: {text}")


SCRIPT_NAME, COMMAND_NAME = "tools/cbind.py", "ceesafec cbind"
compared = 0


def script(arguments: list, ceesafec: str) -> subprocess.CompletedProcess:
    env = dict(os.environ, CEESAFEC=ceesafec)
    return subprocess.run([sys.executable, str(HERE / "cbind.py")] + [str(a) for a in arguments],
                          capture_output=True, text=True, env=env)


def command(arguments: list, ceesafec: str, env: dict = None, clang: bool = True) -> subprocess.CompletedProcess:
    """`ceesafec cbind`, with the clang the script uses unless `clang` is False."""
    named = ["--cc", os.environ.get("CLANG", "clang")] if clang else []
    return subprocess.run([ceesafec, "cbind"] + named + [str(a) for a in arguments], capture_output=True, text=True,
                          env=env)


def twin(path: Path) -> Path:
    """Where the command writes what the script writes to `path`."""
    return path.parent / ("command-" + path.name)


def same_output(what: str, by_script: Path, by_command: Path):
    """The command's file is the script's, byte for byte, but for the generator's name in the first line."""
    global compared
    if not by_command.is_file():
        problem(f"{what}: the script wrote {by_script.name}, the command did not")
        return
    expected = by_script.read_bytes()
    first, _, rest = expected.partition(b"\n")
    if SCRIPT_NAME.encode() not in first:
        problem(f"{what}: the script's first line does not name it: {first!r}")
    expected = first.replace(SCRIPT_NAME.encode(), COMMAND_NAME.encode(), 1) + b"\n" + rest
    found = by_command.read_bytes()
    compared += 1
    if found != expected:
        lines, others = expected.splitlines(), found.splitlines()
        at = next((i for i, (a, b) in enumerate(zip(lines, others)) if a != b), min(len(lines), len(others)))
        problem(f"{what}: {by_command.name} differs from the script's at line {at + 1}:\n"
                f"  script:  {lines[at] if at < len(lines) else b'(end)'!r}\n"
                f"  command: {others[at] if at < len(others) else b'(end)'!r}")


def cbind(arguments: list, ceesafec: str) -> subprocess.CompletedProcess:
    """Runs the script, then the command with the same arguments, its output beside the script's, and compares the
    two; the script's run is returned. The output is the last two arguments: -o FILE, or --out-dir DIRECTORY."""
    flag, output = arguments[-2], Path(arguments[-1])
    what = " ".join(Path(str(a)).name if os.sep in str(a) else str(a) for a in arguments[:-2])
    run = script(arguments, ceesafec)
    other = command(arguments[:-2] + [flag, twin(output)], ceesafec)
    if (run.returncode == 0) != (other.returncode == 0):
        problem(f"{what}: the script exits {run.returncode}, the command {other.returncode}:\n"
                f"{(run.stderr + other.stderr)[:2000]}")
    elif run.returncode != 0:
        # Both refuse: with the same words after their names, up to the first line's end, and the command with 1.
        said, heard = run.stderr.splitlines()[:1], other.stderr.splitlines()[:1]
        if [s.replace("cbind: ", "", 1) for s in said] != [s.replace("ceesafec: ", "", 1) for s in heard]:
            problem(f"{what}: the script says {said}, the command {heard}")
        if other.returncode != 1:
            problem(f"{what}: the command exits {other.returncode}, not 1")
    elif flag == "-o":
        same_output(what, output, twin(output))
    else:
        names = sorted(p.name for p in output.glob("*.cee"))
        if names != sorted(p.name for p in twin(output).glob("*.cee")):
            problem(f"{what}: the command wrote other files than {names}")
        for name in names:
            same_output(what, output / name, twin(output) / name)
    return run


def lines_of(path: Path) -> list:
    return [line.strip() for line in path.read_text().splitlines()]


def declarations(path: Path) -> str:
    return "\n".join(line for line in path.read_text().splitlines() if not line.lstrip().startswith("//"))


def build_and_run(ceesafec: str, program: Path, sources: list, work: Path, flags: list, what: str):
    exe = work / "program"
    cflags = [x for flag in [f"-I{FIXTURE}"] + flags for x in ("--cflag", flag)]
    run = subprocess.run([ceesafec, "build", str(program)] + [str(s) for s in sources]
                         + [str(FIXTURE / "fixture.c")] + cflags + ["-o", str(exe)], capture_output=True, text=True)
    if run.returncode != 0:
        problem(f"{what}: the program does not build:\n{(run.stdout + run.stderr)[:3000]}")
        return
    code = subprocess.run([str(exe)]).returncode
    if code != EXIT:
        problem(f"{what}: the program exits {code}, not {EXIT}")


def behaviour(ceesafec: str, work: Path):
    """What `ceesafec cbind` does that the script does not: its help, its usage errors, the compiler it finds."""
    out = work / "behaviour.cee"
    base = ["--header", "fixture.h", "-I", FIXTURE, "-D", "FX_WIDE=0"]

    run = command(["--help"], ceesafec, clang=False)
    options = ["--header", "-I", "-D", "--target", "--config", "--only", "--with", "--leave", "--cc", "--no-fmt",
               "-o", "--out-dir"]
    if run.returncode != 0 or run.stderr or any(o not in run.stdout for o in options):
        problem(f"cbind --help: exit {run.returncode}, or an option is not listed")
    for flag in ("-h", "--help"):
        run = command(base + [flag], ceesafec)
        if run.returncode != 0 or "usage: ceesafec cbind" not in run.stdout:
            problem(f"cbind … {flag} does not print the command's help")
    run = subprocess.run([ceesafec, "--help"], capture_output=True, text=True)
    if not re.search(r"^  cbind\b", run.stdout, re.M):
        problem("ceesafec --help does not list cbind")

    # Usage errors: exit status 2, the reason and the usage on standard error, nothing written.
    for what, arguments, words in (
            ("no arguments", [], "--header"),
            ("no header", ["-I", FIXTURE, "-o", out], "--header"),
            ("no output", base, "-o FILE"),
            ("-o and --out-dir", base + ["-o", out, "--out-dir", work / "nowhere"], "-o FILE"),
            ("two headers and -o", base + ["--header", "other.h", "-o", out], "-o FILE"),
            ("an unknown option", base + ["--frobnicate", "-o", out], "--frobnicate"),
            ("an option without its value", base + ["-o"], "-o needs a value"),
            ("a file argument", base + ["fixture.h", "-o", out], "fixture.h"),
            ("two headers of one name", ["--header", "fixture.h", "--header", "sub/fixture.h", "-I", FIXTURE,
                                         "--out-dir", work / "nowhere"], "two headers would be written"),
            ("a configuration named twice", base + ["--config", "a=-DX", "--config", "a=-DY", "-o", out], "--config"),
            ("a configuration without a name", base + ["--config", "=-DX", "-o", out], "--config"),
            ("a configuration with an open quote", base + ["--config", "a=\"-DX", "-o", out], "--config")):
        run = command(arguments, ceesafec)
        if run.returncode != 2 or words not in run.stderr or "usage: ceesafec cbind" not in run.stderr or run.stdout:
            problem(f"usage error ({what}): exit {run.returncode}, not 2, or its message is not the expected one:\n"
                    f"{run.stderr[:500]}")
    if out.exists() or (work / "nowhere").exists():
        problem("a usage error wrote output")

    # The option forms: -Idir, -DNAME, --header=h, -ofile are what -I dir, -D NAME, --header h, -o file are.
    spaced, attached = work / "spaced.cee", work / "attached.cee"
    first = command(base + ["-o", spaced], ceesafec)
    second = command(["--header=fixture.h", f"-I{FIXTURE}", "-DFX_WIDE=0", f"-o{attached}"], ceesafec)
    if first.returncode != 0 or second.returncode != 0 or spaced.read_bytes() != attached.read_bytes():
        problem("the attached option forms (-Idir, -DNAME, --header=h, -ofile) do not give what the spaced ones do")

    # A header that is not there: exit status 1, and no usage text.
    run = command(["--header", "no_such.h", "-I", FIXTURE, "-o", out], ceesafec)
    if run.returncode != 1 or "no_such.h is not under any -I directory" not in run.stderr or "usage:" in run.stderr:
        problem(f"a header that is not found: exit {run.returncode}, not 1, or not said:\n{run.stderr[:500]}")
    run = command(base + ["--with", work / "no_such.cee", "-o", out], ceesafec)
    if run.returncode != 1 or "no_such.cee" not in run.stderr:
        problem(f"a --with file that is not found: exit {run.returncode}, not 1, or not said:\n{run.stderr[:500]}")

    # The C compiler. One that is not clang is refused, by --cc or by $CC; one that cannot be run is said so; with
    # none named, the one `build` finds if it is clang, else clang on PATH (there is one: main checked).
    if os.name != "nt":
        fake = work / "notclang"
        fake.write_text("#!/bin/sh\necho 'cc (GCC) 12.2.0'\n")
        fake.chmod(0o755)
        without = {k: v for k, v in os.environ.items() if k != "CC"}
        for what, arguments, env in (("--cc", ["--cc", fake], without), ("$CC", [], dict(without, CC=str(fake)))):
            run = command(arguments + base + ["-o", out], ceesafec, env=env, clang=False)
            if run.returncode != 2 or "is not clang" not in run.stderr or str(fake) not in run.stderr or out.exists():
                problem(f"a C compiler that is not clang ({what}): exit {run.returncode}, not 2, or not said:\n"
                        f"{run.stderr[:500]}")
        run = command(["--cc", work / "no_such_compiler"] + base + ["-o", out], ceesafec, env=without, clang=False)
        if run.returncode != 2 or "cannot run the C compiler" not in run.stderr or out.exists():
            problem(f"a C compiler that cannot be run: exit {run.returncode}, not 2, or not said:\n{run.stderr[:500]}")
        run = command(base + ["-o", out], ceesafec, env=without, clang=False)
        if run.returncode != 0 or out.read_bytes() != spaced.read_bytes():
            problem(f"no compiler named: exit {run.returncode}, or the output is not --cc clang's:\n{run.stderr[:500]}")
        run = command(["--cc", fake] + base + ["-o", out], ceesafec, env=dict(without, CC="clang"), clang=False)
        if run.returncode != 2:
            problem("--cc does not come before $CC")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ceesafec", required=True)
    args = parser.parse_args()
    ceesafec = str(Path(args.ceesafec).resolve())
    if shutil.which(os.environ.get("CLANG", "clang")) is None:
        print("cbind_check: skipped: no clang (the tool reads clang's AST)")
        return 0

    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)

        # 1 and 2: one configuration.
        one = work / "one.cee"
        run = cbind(["--header", "fixture.h", "-I", FIXTURE, "-D", "FX_WIDE=0", "-o", one], ceesafec)
        if run.returncode != 0:
            problem(f"one configuration: cbind failed:\n{run.stderr[:3000]}")
            return 1
        have = lines_of(one)
        for line in DECLARED:
            if line not in have:
                problem(f"one configuration: no line `{line}`")
        code, text = declarations(one), one.read_text()
        for name, why in LEFT_OUT:
            if re.search(rf"\b{name}\b", code):
                problem(f"one configuration: {name} is declared")
            if why and not re.search(rf"//\s+{name}: .*{re.escape(why)}", text):
                problem(f"one configuration: no comment says why {name} is left out ('{why}')")
        if subprocess.run([ceesafec, "fmt", "--check", str(one)], capture_output=True).returncode != 0:
            problem("one configuration: the output is not formatted")
        small = work / "small.cee"       # use.cee needs other.h's struct whole; this needs the one binding alone
        small.write_text("/// Safety: fx_add takes any two integers.\n"
                         "fn main() -> i32 => FX_LIMIT + truncate<i32>(FX_GREEN as u32) + unsafe { fx_add(fx_count, 17) };\n")
        build_and_run(ceesafec, small, [one], work, ["-DFX_WIDE=0"], "one configuration")

        # 4: two headers, one file each; they compile together, and the program uses both.
        both = work / "both"
        run = cbind(["--header", "fixture.h", "--header", "other.h", "-I", FIXTURE, "-D", "FX_WIDE=0",
                     "--out-dir", both], ceesafec)
        if run.returncode != 0:
            problem(f"two headers: cbind failed:\n{run.stderr[:3000]}")
        else:
            first, second = lines_of(both / "fixture.cee"), lines_of(both / "other.cee")
            for line in ("struct other_thing {", "a: i32;", "enum other_mode: u32;", "const OTHER_B: other_mode = 1;",
                         "fn other_get(t: *other_thing) -> i32;", '@ctype("other_cb")'):
                if line not in second:
                    problem(f"two headers: other.cee has no line `{line}`")
            for line in ("struct other_thing;", "enum other_mode: u32;"):
                if line in first:
                    problem(f"two headers: fixture.cee still declares `{line}`, which other.h defines")
            if "struct fx_node;" not in first:
                problem("two headers: fx_node, which neither defines, is not declared")
            build_and_run(ceesafec, FIXTURE / "use.cee", [both / "fixture.cee", both / "other.cee"], work,
                          ["-DFX_WIDE=0"], "two headers")

        # 3: two configurations.
        two = work / "two"
        run = cbind(["--header", "fixture.h", "--header", "other.h", "-I", FIXTURE, "--config", "narrow=-DFX_WIDE=0",
                     "--config", "wide=-DFX_WIDE=1", "--out-dir", two], ceesafec)
        if run.returncode != 0:
            problem(f"two configurations: cbind failed:\n{run.stderr[:3000]}")
        else:
            text, code = (two / "fixture.cee").read_text(), declarations(two / "fixture.cee")
            if "the same in: narrow, wide" not in text:
                problem("two configurations: the output does not name them")
            for name in ("FX_WIDTH", "fx_box"):
                if not re.search(rf"//\s+{name} \(not the same in each\)", text):
                    problem(f"two configurations: {name} is not listed as differing")
            if "FX_WIDTH" in code:
                problem("two configurations: FX_WIDTH is declared")
            if "struct fx_box;" not in code or re.search(r"^\s+origin: fx_point;", code, re.M):
                problem("two configurations: fx_box is not declared by its name alone")
            if "const FX_LIMIT: i32 = 22;" not in code or "fn fx_pick(" not in code:
                problem("two configurations: what is the same in both is not declared")
            # The members differ, so the program's own block declares the struct, and the binding leaves its name out.
            run = cbind(["--header", "fixture.h", "--header", "other.h", "-I", FIXTURE, "--config", "narrow=-DFX_WIDE=0",
                         "--config", "wide=-DFX_WIDE=1", "--leave", "fx_box", "--out-dir", two], ceesafec)
            code = declarations(two / "fixture.cee")
            if run.returncode != 0 or "struct fx_box" in code or "fn fx_pick(" not in code:
                problem("--leave fx_box: the struct is still declared, or what mentions it is not")
            hand = work / "hand.cee"
            hand.write_text('@header("fixture.h")\nextern "C" {\n    struct fx_box {\n        origin: fx_point;\n'
                            '        color: fx_color;\n        notify: extern "C" fn(*mut void) -> i32;\n'
                            '        label: [u8; 16];\n        @cname("type")\n        type_: u8;\n'
                            '        @cfg("FX_WIDE")\n        wide: u64;\n    }\n}\n')
            for flag in ("-DFX_WIDE=0", "-DFX_WIDE=1"):
                build_and_run(ceesafec, FIXTURE / "use.cee", [two / "fixture.cee", two / "other.cee", hand], work, [flag],
                              f"two configurations, {flag}")

        # 5: --only, and --with.
        only = work / "only.cee"
        run = cbind(["--header", "fixture.h", "-I", FIXTURE, "-D", "FX_WIDE=0", "--only", "fx_pick,FX_LIMIT", "-o", only],
                    ceesafec)
        code = declarations(only) if run.returncode == 0 else ""
        for name, wanted in (("fn fx_pick(", True), ("struct fx_box {", True), ("struct fx_point {", True),
                             ("enum fx_color: u32;", True), ("FX_LIMIT", True), ("fx_add", False), ("FX_MASK", False),
                             ("fx_bits", False)):
            if (name in code) != wanted:
                problem(f"--only fx_pick,FX_LIMIT: `{name}` is {'missing' if wanted else 'declared'}")
        run = cbind(["--header", "fixture.h", "-I", FIXTURE, "-D", "FX_WIDE=0", "--only", "no_such_name", "-o", only],
                    ceesafec)
        if run.returncode == 0 or "no_such_name" not in run.stderr:
            problem("--only with a name the header does not declare is not refused")
        with_ = work / "with.cee"
        run = cbind(["--header", "fixture.h", "-I", FIXTURE, "-D", "FX_WIDE=0", "--with", both / "other.cee",
                     "-o", with_], ceesafec)
        code = declarations(with_) if run.returncode == 0 else "struct other_thing;"
        if "struct other_thing;" in code or "enum other_mode" in code or "struct fx_node;" not in code:
            problem("--with other.cee: its types are still declared, or one it does not declare is not")

        # A header clang cannot read is an error, with clang's words.
        bad = work / "bad.h"
        bad.write_text("struct broken { no_such_type x; };\n")
        run = cbind(["--header", "bad.h", "-I", work, "-o", work / "bad.cee"], ceesafec)
        if run.returncode == 0 or "no_such_type" not in run.stderr:
            problem("a header that does not compile is not refused")

        # 7: the book's header, the generator's own layout, other targets (an enum is `int` for Windows).
        clock = HERE.parent / "book" / "projects" / "ch20" / "clock"
        if (clock / "c" / "clock.h").is_file():
            run = cbind(["--header", "clock.h", "-I", clock / "c", "-o", work / "clock.cee"], ceesafec)
            committed = (clock / "src" / "clock.cee").read_bytes()
            if run.returncode != 0:
                problem(f"the book's clock.h: cbind failed:\n{run.stderr[:3000]}")
            elif committed not in ((work / "clock.cee").read_bytes(), twin(work / "clock.cee").read_bytes()):
                problem("the book's clock/src/clock.cee is neither generator's output for clock/c/clock.h")
        for name, flags in (("plain", ["--no-fmt"]), ("elf", ["--target", "x86_64-unknown-none-elf"]),
                            ("arm", ["--target", "aarch64-unknown-none-elf", "--no-fmt"]),
                            ("windows", ["--target", "x86_64-pc-windows-msvc"])):
            run = cbind(["--header", "fixture.h", "--header", "other.h", "-I", FIXTURE, "-D", "FX_WIDE=1"] + flags
                        + ["--out-dir", work / name], ceesafec)
            if run.returncode != 0:
                problem(f"{' '.join(flags)}: cbind failed:\n{run.stderr[:3000]}")
        if "enum fx_color: i32;" not in lines_of(work / "windows" / "fixture.cee"):
            problem("--target x86_64-pc-windows-msvc: fx_color is not an i32")
        # A declaration longer than a line, which ceesafec fmt breaks and --no-fmt does not.
        (work / "long.h").write_text("int fx_long(" + ", ".join(f"const char *parameter_number_{i}" for i in range(9))
                                     + ");\n")
        for name, flags in (("long.cee", []), ("long-plain.cee", ["--no-fmt"])):
            run = cbind(["--header", "long.h", "-I", work] + flags + ["-o", work / name], ceesafec)
            if run.returncode != 0:
                problem(f"long.h {' '.join(flags)}: cbind failed:\n{run.stderr[:3000]}")
        if (work / "long.cee").read_bytes() == (work / "long-plain.cee").read_bytes():
            problem("--no-fmt: the output is the formatted one, so the test of it compares nothing")

        # 8: the command's own behaviour.
        behaviour(ceesafec, work)

    # 6: the keywords.
    sys.path.insert(0, str(HERE))
    import cbind as tool
    lexer = set(re.findall(r'TokenKind\.\w+Keyword => "([a-z_][A-Za-z_]*)",',
                           (HERE.parent / "src" / "syntax" / "token.cee").read_text()))
    if tool.KEYWORDS != lexer:
        problem(f"cbind.py's KEYWORDS differ from the lexer's: {sorted(tool.KEYWORDS ^ lexer)}")

    print(f"cbind_check: {len(DECLARED)} declarations, {len(LEFT_OUT)} left out, 2 configurations, 2 headers, "
          f"--only, --with, --leave; {compared} files of ceesafec cbind compared with the script's; the command's "
          f"help, usage errors and compiler: {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
