#!/usr/bin/env python3
"""Child processes that run beside their parent (ADR 0169): checks the corpus cannot make, since its runner starts no
helper programs.

A program is built that starts itself again as its children (`Process.executable`, `Process.spawn`):

1. a child that exits with a status: `wait` gives it, and gives it again; `terminate` and `kill` then do nothing;
2. a child that subscribes to `ProcessSignals` and is asked to end (`terminate`): it is told, and exits 0;
3. a child that is ended at once (`kill`): `ChildExit.Signal(9)`;
4. a program that does not exist: an error, and no child;
5. a child that is dropped while it runs, with nothing for its streams: the parent ends first, and the child ends
   by itself three seconds later, not a zombie.

The interpreter starts no children: there `Process.spawn` is `IoError.Unsupported`, `Process.id()` is 0 and
`Process.executable()` is an error, which is checked too.

On Windows the third child ends with status 1, not a signal, and the children are found by the program's name
(tools/child_check.py --write DIR writes the programs, to try them by hand).

usage: python3 tools/child_check.py --compiler out/ceesafec
"""

import argparse
import os
import subprocess
import sys
import tempfile
import time

PARENT = r'''// The parent starts itself again as children: `kids child <how>`.
fn child(how: StringView) -> i32 {
    if (how == "exits") {
        return 7;
    }
    if (how == "listens") {
        let signals = ProcessSignals.subscribe()!;
        Console.writeLine("child listening");
        Console.flush();
        let asked = signals.wait();
        Console.writeLine(if (asked is ProcessSignal.Terminate) "child told to terminate" else "child told something else");
        Console.flush();
        return 0;
    }
    // "sleeps": for three seconds, unless it is ended first.
    Console.writeLine("child sleeping");
    Console.flush();
    pause(3000);
    return 3;
}

async fn nap(ms: u64) -> Result<void, AsyncError> => await Timer.sleep(ms);

fn pause(ms: u64) {
    let _ = Async.run(nap(ms));
}

fn describe(exit: ChildExit) {
    match (exit) {
        ChildExit.Status(code) => {
            Console.write("status ");
            Console.writeLine(code as i64);
        }
        ChildExit.Signal(number) => {
            Console.write("signal ");
            Console.writeLine(number as i64);
        }
    }
}

fn start(me: StringView, how: StringView, options: ChildOptions) -> ChildProcess {
    var arguments = Vector<StringView>.create();
    arguments.push("child")!;
    arguments.push(how)!;
    return Process.spawn(me, arguments, options)!;
}

fn main() {
    match (Process.argument(1)) {
        Some(word) => {
            if (word == "child") {
                Process.exit(child(Process.argument(2) ?? ""));
            }
        }
        null => {}
    }
    let me = Process.executable()!;
    Console.writeLine(if (Process.id() > 1) "has an id" else "no id");
    // One that exits by itself.
    var first = start(me.view(), "exits", ChildOptions.inherit());
    Console.writeLine(if (first.id() != Process.id() && first.id() > 1) "the child has another id" else "ids are wrong");
    describe(first.wait()!);
    describe(first.wait()!); // the exit is kept
    Console.writeLine(if (first.terminate().isOk() && first.kill().isOk()) "ended: nothing to signal" else "signalled the dead");
    // One that is asked to end, and hears it.
    var second = start(me.view(), "listens", ChildOptions.inherit());
    Console.writeLine(if (second.poll()!.isNull()) "still running" else "ended early");
    pause(300);
    second.terminate()!;
    describe(second.wait()!);
    // One that is ended at once.
    var third = start(me.view(), "sleeps", ChildOptions { input: ChildStream.Inherit, output: ChildStream.Null, error: ChildStream.Null });
    pause(200);
    third.kill()!;
    describe(third.wait()!);
    // A program that is not there.
    var none = Vector<StringView>.create();
    Console.writeLine(if (Process.spawn("no-such-program-anywhere", none, ChildOptions.quiet()).isErr()) "not found" else "found?");
    // One that is dropped while it runs: the parent ends, and the child goes on to its own end.
    let fourth = start(me.view(), "sleeps", ChildOptions.quiet());
    Console.writeLine("done");
}
'''

INTERPRETED = r'''fn main() {
    Console.writeLine(Process.id());
    Console.writeLine(if (Process.executable().isErr()) "no executable" else "an executable");
    var none = Vector<StringView>.create();
    match (Process.spawn("true", none, ChildOptions.quiet())) {
        Ok(_) => { Console.writeLine("started"); }
        Err(e) => { Console.writeLine(if (e is IoError.Unsupported) "unsupported" else "another error"); }
    }
}
'''

EXPECTED_PARENT = [
    "has an id",
    "the child has another id",
    "status 7",
    "status 7",
    "ended: nothing to signal",
    "still running",
    "status 0",
    "signal 9",
    "not found",
    "done",
]
EXPECTED_CHILDREN = ["child listening", "child told to terminate"]
DEADLINE = 60.0


WINDOWS = os.name == "nt"


def children_of(exe):
    """The ids of the running copies of `exe` that are children (`exe child ...`). On Windows, of every running
    copy: the parent has ended when this is asked."""
    if WINDOWS:
        listed = subprocess.run(["tasklist", "/FO", "CSV", "/NH", "/FI", f"IMAGENAME eq {os.path.basename(exe)}"],
                                capture_output=True, text=True).stdout
        return [line.split(",")[1].strip('"') for line in listed.splitlines() if line.startswith('"')]
    listed = subprocess.run(["ps", "-eo", "pid=,args="], capture_output=True, text=True).stdout
    return [line.split()[0] for line in listed.splitlines() if f"{exe} child" in line]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--write", metavar="DIR", help="only write the programs to DIR (to try them by hand)")
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    if args.write:
        os.makedirs(args.write, exist_ok=True)
        for stem, source in (("kids", PARENT), ("interpreted", INTERPRETED)):
            with open(os.path.join(args.write, stem + ".cee"), "w", encoding="utf-8") as f:
                f.write(source)
        print(f"child_check: wrote 2 programs to {args.write}")
        return 0
    # Windows has no signal to end a process with: a child that is killed has status 1 (type-system.md §26.16).
    expected_parent = [("status 1" if WINDOWS and line == "signal 9" else line) for line in EXPECTED_PARENT]
    problems = []
    with tempfile.TemporaryDirectory(prefix="ceesafe-child-") as work:
        path = os.path.join(work, "kids.cee")
        with open(path, "w", encoding="utf-8") as f:
            f.write(PARENT)
        # A name of this run's own: on Windows the children are found by it.
        exe = os.path.join(work, f"kids{os.getpid()}.exe" if WINDOWS else "kids")
        b = subprocess.run([compiler, "build", path, "-o", exe], capture_output=True, text=True)
        if b.returncode != 0:
            problems.append(f"kids: build failed: {b.stderr.strip()[:600]}")
        else:
            p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                out, err = p.communicate(timeout=DEADLINE)
            except subprocess.TimeoutExpired:
                p.kill()
                out, err = p.communicate()
                problems.append("kids: did not end in time")
            lines = out.splitlines()
            parent = [l for l in lines if not l.startswith("child ")]
            children = [l for l in lines if l.startswith("child ") and l != "child sleeping"]
            if parent != expected_parent:
                problems.append(f"kids: the parent wrote {parent!r}, expected {expected_parent!r} ({err.strip()[:300]!r})")
            if children != EXPECTED_CHILDREN:
                problems.append(f"kids: the children wrote {children!r}, expected {EXPECTED_CHILDREN!r}")
            if p.returncode != 0:
                problems.append(f"kids: the parent's status was {p.returncode}")
            # The dropped child goes on after its parent, to the end of its three seconds.
            end = time.monotonic() + 10.0
            while children_of(exe) and time.monotonic() < end:
                time.sleep(0.1)
            left = children_of(exe)
            if left:
                problems.append(f"kids: children still there after the parent ended: {left}")
                for pid in left:
                    subprocess.run(["taskkill", "/F", "/PID", pid] if WINDOWS else ["kill", "-9", pid], capture_output=True)
        path = os.path.join(work, "interpreted.cee")
        with open(path, "w", encoding="utf-8") as f:
            f.write(INTERPRETED)
        r = subprocess.run([compiler, "run", "--interp", path], capture_output=True, text=True, timeout=DEADLINE)
        if r.stdout.splitlines() != ["0", "no executable", "unsupported"]:
            problems.append(f"interpreted: wrote {r.stdout!r} ({r.stderr.strip()[:300]!r})")
    for problem in problems:
        print(f"FAIL {problem}")
    print(f"child_check: 6 cases: {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
