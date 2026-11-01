#!/usr/bin/env python3
"""Debugging a `ceesafec build -g` program with editors/debug (docs/p8-plan.md S6).

Under lldb on macOS, gdb elsewhere (or the one given): breakpoints at source lines, `ceesafe-locals` and `ceesafe-bt`
showing CeeSafe names, a breakpoint on ceesafe_panic stopping at the failed check, the same through `--split` with
CEESAFE_PROGRAM, `ceesafe-tasks` listing an async program's tasks (L1 A3), and the name demangling of ceesafe_names.py; a breakpoint on ceesafe_panic stops at the failed check. The assertions are DebuggerTests'.

usage: python3 tools/debugger_check.py --ceesafec out/ceesafec [--debugger lldb|gdb]
"""
import argparse
import os
import platform
import shutil
import subprocess
import sys
import tempfile

SCRIPTS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "editors", "debug")

# Line numbers matter: the breakpoints name lines 6 and 12, and the calls are on line 18.
PROGRAM = """struct Point { x: i32; y: i32; }

struct Counter {
    n: i32;
    public fn bump(borrow mut this, by: i32) {
        this.n += by;
    }
}

fn length(p: Point) -> i32 {
    let sum = p.x + p.y;
    return sum;
}

fn main() {
    let p = Point { x: 3, y: 4 };
    var c = Counter { n: 0 };
    c.bump(length(p));
    Console.writeLine(c.n);
}
"""

PANICKING = """fn grow(level: u8) -> u8 {
    return level + 200;
}

fn main() {
    var level: u8 = 100;
    level = grow(level);
    Console.writeLine(level);
}
"""

# Line numbers matter: the breakpoint is in mark (line 7); the scope is on line 18, the children wait on lines 20
# and 25.
TASKS = """enum Failure {
    OutOfMemory,
    Bad,
}

fn mark(n: u32) -> u32 {
    return n + 1;
}

async fn pause(ms: u64) -> Result<void, Failure> {
    match (await Timer.sleep(ms)) {
        Ok() => { return Ok(); }
        Err(_) => { return Err(Failure.Bad); }
    }
}

async fn gather() -> Result<void, Failure> {
    await Async.scope(Failure.OutOfMemory, (s) => {
        s.spawn(async () => {
            await pause(10)?;
            let m = mark(1);
            return Ok();
        });
        s.spawn(async () => {
            await pause(500)?;
            return Ok();
        });
    })?;
    return Ok();
}

fn main() {
    match (Async.run(gather())) {
        Ok() => { Console.writeLine("done"); }
        Err(_) => { Console.writeLine("failed"); }
    }
}
"""

def debug(cee, debugger, work, name, text, commands, extra=(), env=None):
    with open(os.path.join(work, f"{name}.cee"), "w") as f:
        f.write(text)
    build = subprocess.run([cee, "build", "-g", *extra, f"{name}.cee", "-o", name], cwd=work, capture_output=True,
                           text=True)
    if build.returncode != 0:
        raise SystemExit(f"build of {name} failed:\n{build.stderr}")
    if debugger == "lldb":
        argv = ["lldb", "--batch", "-o", f"command script import {os.path.join(SCRIPTS, 'ceesafe_lldb.py')}"]
    else:
        argv = ["gdb", "-batch", "-nx", "-ex", f"source {os.path.join(SCRIPTS, 'ceesafe_gdb.py')}"]
    for command in commands:
        argv += ["-o" if debugger == "lldb" else "-ex", command]
    argv += ["--", f"./{name}"] if debugger == "lldb" else [f"./{name}"]
    run = subprocess.run(argv, cwd=work, capture_output=True, text=True, timeout=120, env=env)
    return run.stdout


def expect(problems, label, text, wanted):
    for w in wanted:
        if w not in text:
            problems.append(f"{label}: missing {w!r}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ceesafec", required=True)
    parser.add_argument("--debugger", choices=("lldb", "gdb"))
    options = parser.parse_args()
    cee = os.path.abspath(options.ceesafec)
    order = ["lldb", "gdb"] if platform.system() == "Darwin" else ["gdb", "lldb"]
    debugger = options.debugger or next((d for d in order if shutil.which(d)), None)
    problems = []
    sys.path.insert(0, SCRIPTS)
    import ceesafe_names
    for c, name in (("ceeF_length", "length"), ("ceeM_7Counter_bump", "Counter.bump"),
                    ("ceeM_12SpinLock_u64_create", "SpinLock_u64.create"), ("ceeAR_ceeF_twice", "twice"),
                    ("ceeAR_ceeCB_250", "async closure"), ("main", "main")):
        if ceesafe_names.function(c) != name:
            problems.append(f"demangling: {c} gave {ceesafe_names.function(c)!r}, not {name!r}")
    checked = ["demangling"]
    if debugger is None:
        print("debugger_check: no lldb or gdb; only the demangling was checked")
    else:
        work = tempfile.mkdtemp(prefix="debugger-")
        try:
            breakAt = "b {}" if debugger == "lldb" else "break {}"
            commands = [breakAt.format("demo.cee:12"), breakAt.format("demo.cee:6"), "run", "ceesafe-locals",
                        "ceesafe-bt", "continue", "ceesafe-locals", "ceesafe-bt"]
            wanted = ["p = { x: 3, y: 4 }", "sum = 7", "length at demo.cee:12", "main at demo.cee:18", "by = 7",
                      "Counter.bump at demo.cee:6"]
            text = debug(cee, debugger, work, "demo", PROGRAM, commands)
            expect(problems, "breakpoints", text, wanted)
            locals_ = [line.split(" ")[0] for line in text.split("\n") if " = " in line]
            if any(n.startswith("t_") for n in locals_):
                problems.append("breakpoints: a compiler temporary was shown")
            env = {**os.environ, "CEESAFE_PROGRAM": "demo"}
            text = debug(cee, debugger, work, "demo", PROGRAM, commands, ["--split", "--program", "demo"], env)
            expect(problems, "split", text, wanted)
            text = debug(cee, debugger, work, "panic", PANICKING,
                         ["b ceesafe_panic" if debugger == "lldb" else "break ceesafe_panic", "run", "ceesafe-bt"])
            expect(problems, "panic", text, ["grow at panic.cee:2", "main at panic.cee:7"])
            text = debug(cee, debugger, work, "tasks", TASKS,
                         [breakAt.format("tasks.cee:7"), "run", "ceesafe-tasks", "ceesafe-bt"])
            expect(problems, "tasks", text, ["gather at tasks.cee:18 (waiting for its scope)\n"
                                             "  async closure at tasks.cee:20 (running)\n"
                                             "  async closure at tasks.cee:25 (waiting for a deadline)",
                                             "mark at tasks.cee:7\nasync closure at tasks.cee:21"])
            checked += ["breakpoints", "split", "panic", "tasks"]
        finally:
            shutil.rmtree(work, ignore_errors=True)
    for p in problems:
        print(p)
    print(f"debugger_check ({debugger or 'none'}): {', '.join(checked)}: {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
