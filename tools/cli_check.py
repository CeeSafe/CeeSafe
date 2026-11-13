#!/usr/bin/env python3
"""Checks ceesafec's command line against spec/tooling.md §4.4 and §12.

1. Nothing to work on: every command that works on files or a package, run with no files outside a package, in a
   package without sources, and in a package whose manifest names a C file that is not there, is a usage error:
   exit status 2, the message and then the usage text on standard error.
2. The help texts: every option src/main.cee reads is at the start of a line of `ceesafec --help` or of
   `ceesafec test --help`, or is one of §12.2's internal options; no internal option is in a help text.
3. The table of §12.1: for each option, the commands that accept it (that do not answer `unknown option`) are
   exactly the ones the table names, found by running parse, check, build, run, fix and test with it.
4. The allocation variables (§12.3): a program built and run with CEESAFE_FAIL_ALLOC_AT=k, and the same program
   under `run --interp`, print the same and exit the same for every k up to its attempts and one past them, and
   ceesafec itself never panics, in `check` and `build` too; with CEESAFE_ALLOC_REPORT=1 the first report under
   `run --interp` is the program's, the one its build prints.

usage: python3 tools/cli_check.py --compiler out/ceesafec
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OPTION = r"--?[A-Za-z][A-Za-z0-9-]*"
COMMANDS = ["parse", "check", "build", "run", "fix", "test"]
# A value each option that takes one accepts, so that the reader goes on to the next word.
VALUES = {"--emit": "exe", "-o": "x", "--exports-header": "x.h", "--cc": "cc", "--cflag": "-O1", "--ld": "ld",
          "--ar": "ar", "--program": "p", "--profile": "hosted", "--kernel": "cosmoos", "--filter": "x",
          "--jobs": "2", "--timeout": "5", "--sweep": "alloc", "--format": "text", "--ceesafe-harness": "h.cee",
          "--package": "p", "--diagnostics-format": "text"}
REPORT = re.compile(r"ceesafe alloc: attempts=(\d+) live=(\d+)")

# Six allocation attempts and two more for the vector that keeps them, each one's failure taken: no panic at any k.
PROGRAM = """fn main() {
    var made: i32 = 0;
    var failed: i32 = 0;
    var kept = Vector<Vector<u8>>.create();
    var round: i32 = 0;
    while (round < 6) {
        match (Vector<u8>.withCapacity(8)) {
            Ok(v) => {
                made += 1;
                match (kept.push(v)) {
                    Ok(_) => {}
                    Err(_) => { failed += 1; }
                }
            }
            Err(_) => { failed += 1; }
        }
        round += 1;
    }
    Console.writeLine(made);
    Console.writeLine(failed);
    Console.writeLine(kept.length);
}
"""


def run(argv, cwd, extra=None):
    env = {k: v for k, v in os.environ.items() if not k.startswith("CEESAFE_")}
    env.update(extra or {})
    return subprocess.run(argv, cwd=cwd, env=env, capture_output=True, text=True, timeout=300,
                          stdin=subprocess.DEVNULL)


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def read(path):
    with open(os.path.join(ROOT, *path.split("/")), encoding="utf-8") as f:
        return f.read()


def manifest_above(directory):
    while True:
        if os.path.exists(os.path.join(directory, "ceesafe.toml")):
            return True
        parent = os.path.dirname(directory)
        if parent == directory:
            return False
        directory = parent


def nothing_to_work_on(compiler, work, problems):
    """1. Status 2 and the usage text, whatever the command."""
    empty, nosrc, missing = (os.path.join(work, name) for name in ("empty", "nosrc", "cmissing"))
    os.makedirs(empty)
    write(os.path.join(nosrc, "ceesafe.toml"), '[package]\nname = "nosrc"\n')
    os.makedirs(os.path.join(nosrc, "src"))
    write(os.path.join(missing, "ceesafe.toml"), '[package]\nname = "cmissing"\nc = ["src/missing.c"]\n')
    write(os.path.join(missing, "src", "main.cee"), "fn main() {\n}\n")
    file_commands = ["parse", "check", "check --all", "check --package x", "check --whole-program", "build",
                     "run --interp", "test", "test --list", "fix", "fmt", "fmt --check", "doc", "doc --missing"]
    cases = []
    if manifest_above(os.path.realpath(empty)):
        problems.append(f"a ceesafe.toml lies above the temporary directory {empty}: the first case cannot be run")
    else:
        cases += [(empty, c, "no CeeSafe input files") for c in file_commands]
        cases += [(empty, c, "no ceesafe.toml here or above") for c in ("fetch", "publish", "agents")]
    cases += [(nosrc, c, "has no .cee files under src") for c in file_commands if c.split()[0] not in ("fmt", "doc")]
    cases += [(nosrc, c, "no CeeSafe input files") for c in file_commands if c.split()[0] in ("fmt", "doc")]
    cases += [(missing, c, "cannot read") for c in ("build", "run --interp", "test")]
    for directory, command, words in cases:
        p = run([compiler] + command.split(), directory)
        first = p.stderr.split("\n")[0]
        if p.returncode != 2 or words not in first or "\nusage: ceesafec" not in p.stderr or p.stdout:
            problems.append(f"'{command}' in {os.path.basename(directory)}: status {p.returncode}, not 2 with "
                            f"'{words}' and the usage text: {p.stderr[:160]!r}")
    return len(cases)


def source_options():
    found = set(re.findall(r'"(' + OPTION + r')"', read("src/main.cee"))) - {"--help", "-h", "--version"}
    if len(found) < 40:
        raise SystemExit(f"cli_check: src/main.cee has {len(found)} option literals: its shape changed")
    return found


def help_options(compiler, arguments):
    """The options at the start of a line of a help text, as tools/book_appendix_check.py reads them."""
    p = run([compiler] + arguments, ROOT)
    found = set()
    for line in p.stdout.split("\n"):
        if re.match(r"\s+-", line):
            head = re.split(r"\s{2,}", line.strip())[0]
            found.update(re.findall(r"(?<![\w-])(" + OPTION + r")", head))
    if p.returncode != 0 or not found:
        raise SystemExit(f"cli_check: `ceesafec {' '.join(arguments)}` prints no option")
    return found


def section(text, start, end):
    a = text.find(start)
    b = text.find(end, a + 1)
    if a < 0 or b < 0:
        raise SystemExit(f"cli_check: spec/tooling.md has no '{start}' before '{end}'")
    return text[a:b]


def help_texts(compiler, spec, problems):
    """2. Every option is in a help text or internal, and no internal one is in a help text."""
    in_source = source_options()
    listed = help_options(compiler, ["--help"]) | help_options(compiler, ["test", "--help"])
    internal = set(re.findall(r"^\| `(" + OPTION + r")", section(spec, "### 12.2 ", "### 12.3 "), re.M))
    if not internal:
        problems.append("spec/tooling.md §12.2 lists no internal option")
    for option in sorted(in_source - listed - internal):
        problems.append(f"{option}: read by src/main.cee, but in neither help text nor in tooling.md §12.2")
    for option in sorted(internal & listed):
        problems.append(f"{option}: internal by tooling.md §12.2, but a help text lists it")
    for option in sorted(internal - in_source):
        problems.append(f"{option}: internal by tooling.md §12.2, but src/main.cee does not read it")
    for option in sorted(help_options(compiler, ["test", "--help"]) - in_source):
        problems.append(f"{option}: in `ceesafec test --help`, but src/main.cee does not read it")
    return len(in_source)


def accepted_by(compiler, work, option):
    """The commands that read `option` and go on to the next word."""
    commands = []
    for command in COMMANDS:
        argv = [compiler, command, option] + ([VALUES[option]] if option in VALUES else []) + ["--cli-check-end"]
        first = run(argv, work).stderr.split("\n")[0]
        if first == "ceesafec: unknown option '--cli-check-end'":
            commands.append(command)
        elif first != f"ceesafec: unknown option '{option}'":
            commands.append(f"{command} ({first[:80]})")
    return commands


def option_table(compiler, work, spec, problems):
    """3. §12.1's second column is what the commands do."""
    table = {}
    for line in section(spec, "### 12.1 ", "### 12.2 ").split("\n"):
        cells = [cell.strip() for cell in line.split("|")]
        if len(cells) < 4 or not cells[1].startswith("`-"):
            continue
        for option in re.findall(r"`(" + OPTION + r")`", cells[1]):
            if option in table:
                problems.append(f"tooling.md §12.1 has two rows for {option}")
            table[option] = [c.strip() for c in cells[2].split(",")]
    in_source = source_options()
    for option in sorted(in_source - set(table)):
        problems.append(f"{option}: read by src/main.cee, but tooling.md §12.1 has no row for it")
    for option in sorted(set(table) - in_source):
        problems.append(f"{option}: a row of tooling.md §12.1, but src/main.cee does not read it")
    for option in sorted(in_source & set(table)):
        found = accepted_by(compiler, work, option)
        if found != table[option]:
            problems.append(f"{option}: accepted by {', '.join(found) or 'no command'}; tooling.md §12.1 says "
                            f"{', '.join(table[option])}")
    return len(table)


def allocation_variables(compiler, work, problems):
    """4. CEESAFE_FAIL_ALLOC_AT reaches the program and never the compiler; the program's report comes first."""
    source = os.path.join(work, "attempts.cee")
    write(source, PROGRAM)
    exe = os.path.join(work, "attempts")
    b = run([compiler, "build", source, "-o", exe], work)
    if b.returncode != 0:
        problems.append(f"allocation variables: the build failed: {b.stderr[-300:]}")
        return 0
    if os.name == "nt":
        exe += ".exe"
    native = run([exe], work, {"CEESAFE_ALLOC_REPORT": "1"})
    m = REPORT.search(native.stderr)
    if native.returncode != 0 or m is None or int(m.group(1)) < 6:
        problems.append(f"allocation variables: the built program's report: {native.stderr[-200:]!r}")
        return 0
    attempts = int(m.group(1))
    interpreted = run([compiler, "run", "--interp", source], work, {"CEESAFE_ALLOC_REPORT": "1"})
    reports = REPORT.findall(interpreted.stderr)
    if interpreted.returncode != 0 or interpreted.stdout != native.stdout or not reports or \
            reports[0] != (m.group(1), m.group(2)):
        problems.append(f"CEESAFE_ALLOC_REPORT=1 run --interp: the first report is not the program's "
                        f"(attempts={attempts} live={m.group(2)}): {interpreted.stderr[-200:]!r}")
    outputs = set()
    for k in range(1, attempts + 2):
        setting = {"CEESAFE_FAIL_ALLOC_AT": str(k)}
        built = run([exe], work, setting)
        interp = run([compiler, "run", "--interp", source], work, setting)
        outputs.add(built.stdout)
        if "ceesafe panic" in interp.stderr or (interp.returncode, interp.stdout) != (built.returncode, built.stdout):
            problems.append(f"CEESAFE_FAIL_ALLOC_AT={k} run --interp: status {interp.returncode}, "
                            f"{interp.stdout!r} {interp.stderr[-160:]!r}; built: status {built.returncode}, "
                            f"{built.stdout!r}")
    if len(outputs) < 3:
        problems.append(f"allocation variables: the failed attempt changed the program's output {len(outputs)} "
                        "ways only: the check is out of date")
    for k in (1, 2, 1000, 200000):
        setting = {"CEESAFE_FAIL_ALLOC_AT": str(k)}
        for command in (["check", source], ["parse", source], ["build", source, "-o", exe + "-again"],
                        ["--version"], ["explain", "N0249"]):
            p = run([compiler] + command, work, setting)
            if p.returncode != 0 or "ceesafe panic" in p.stderr:
                problems.append(f"CEESAFE_FAIL_ALLOC_AT={k} {' '.join(command[:1])}: status {p.returncode}: "
                                f"{p.stderr[-200:]!r}")
    return attempts


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    spec = read("spec/tooling.md")
    problems = []
    with tempfile.TemporaryDirectory(prefix="ceesafe-cli-") as work:
        cases = nothing_to_work_on(compiler, work, problems)
        options = help_texts(compiler, spec, problems)
        matrix = os.path.join(work, "matrix")
        os.makedirs(matrix)
        rows = option_table(compiler, matrix, spec, problems)
        attempts = allocation_variables(compiler, work, problems)
    print(f"cli_check: {cases} commands with nothing to work on, {options} options against the help texts, "
          f"{rows} against tooling.md §12.1, {attempts} failed attempts built and interpreted; "
          f"{len(problems)} problems")
    for problem in problems:
        print("  FAIL " + problem)
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
