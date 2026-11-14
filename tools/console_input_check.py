#!/usr/bin/env python3
"""An interactive program can be played, built and in the interpreter (ADR 0106): checks the corpus cannot make, since
its runner gives a program its whole input at once and reads its output at the end.

Each case runs natively and with `ceesafec run --interp`, with a deadline for every wait:

1. a dialogue through pipes: the program writes a prompt that ends no line and calls `Console.readLine`; the check
   reads the prompt BEFORE it writes the answer, three times, one answer arriving in two pieces and ending `\\r\\n`;
   then it closes the input and reads the program's last line;
2. `Console.flush()` writes what the program wrote, through a pipe, while the program waits for something that is not
   standard input (opening a FIFO), and a finished line reaches a terminal (a pty) without a flush;
3. `AsyncConsole` writes to standard output when it is /dev/null, a character device that macOS's poll cannot wait
   on (it reports POLLNVAL for the open descriptor, which was taken for a closed one: Err(Io(Broken pipe))).

POSIX only (pipes under select, FIFOs, ptys): elsewhere it reports that it was skipped.

usage: python3 tools/console_input_check.py --compiler out/ceesafec
"""

import argparse
import os
import select
import subprocess
import sys
import tempfile
import time

DIALOGUE = """
fn main() {
    var line = String.create();
    loop {
        Console.write("? ");
        if (!Console.readLine(mut line)!) {
            break;
        }
        Console.write("[");
        Console.write(line.view());
        Console.writeLine("]");
    }
    Console.writeLine("end");
}
"""

# Writes, then waits for the FIFO named by CEESAFE_CONSOLE_FIFO to have a writer: `flush` with CEESAFE_CONSOLE_FLUSH
# set (the text ends no line), else a whole line and no flush.
WAITING = """
fn main() {
    if (Environment.variable("CEESAFE_CONSOLE_FLUSH").isSome()) {
        Console.write("ready");
        Console.flush();
    } else {
        Console.writeLine("ready");
    }
    var gate = File.open(Environment.variable("CEESAFE_CONSOLE_FIFO")!)!;
    var byte = [0u8; 1];
    let count = gate.read(mut byte[..])!;
    Console.writeLine(count);
}
"""

# Writes a line through AsyncConsole and says on standard error whether the write succeeded.
DISCARDED = """
async fn say() -> Result<void, AsyncError> {
    await AsyncConsole.writeLine("discarded")?;
    return Ok();
}

fn main() {
    match (Async.run(say())) {
        Ok() => { Console.writeError("written"); }
        Err(_) => { Console.writeError("failed"); }
    }
}
"""

DEADLINE = 20.0


class Reader:
    """Reads a descriptor up to a marker, with a deadline."""

    def __init__(self, fd):
        self.fd, self.seen = fd, b""

    def until(self, marker):
        end = time.monotonic() + DEADLINE
        while marker not in self.seen:
            left = end - time.monotonic()
            ready = select.select([self.fd], [], [], max(left, 0))[0] if left > 0 else []
            if not ready:
                return None
            try:
                piece = os.read(self.fd, 4096)
            except OSError:  # a pty's master at its end (EIO on Linux)
                piece = b""
            if not piece:
                return None
            self.seen += piece
        at = self.seen.index(marker) + len(marker)
        text, self.seen = self.seen[:at], self.seen[at:]
        return text


def dialogue(argv, problems, name):
    p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    out = Reader(p.stdout.fileno())
    try:
        steps = [([b"one\n"], b"[one]\n? "), ([b"par", b"tial\r\n"], b"[partial]\n? "), ([b"\n"], b"[]\n? ")]
        if out.until(b"? ") != b"? ":
            problems.append(f"{name}: no prompt before the first answer was written (seen {out.seen!r})")
            return
        for pieces, expected in steps:
            for piece in pieces:
                p.stdin.write(piece)
                p.stdin.flush()
                time.sleep(0.2 if len(pieces) > 1 else 0)
            if out.until(b"? ") != expected:
                problems.append(f"{name}: after {b''.join(pieces)!r}, expected {expected!r} before the next answer "
                                f"(seen {out.seen!r})")
                return
        p.stdin.close()
        if out.until(b"end\n") != b"end\n" or p.wait(DEADLINE) != 0:
            problems.append(f"{name}: no 'end' and exit 0 at the end of the input (seen {out.seen!r})")
    finally:
        p.kill()
        p.wait()


def waiting(argv, problems, name, work, flush, terminal):
    """The program's 'ready' is read while it still waits for the FIFO."""
    import pty

    fifo = os.path.join(work, f"gate-{len(os.listdir(work))}")
    os.mkfifo(fifo)
    env = dict(os.environ, CEESAFE_CONSOLE_FIFO=fifo)
    env.pop("CEESAFE_CONSOLE_FLUSH", None)
    if flush:
        env["CEESAFE_CONSOLE_FLUSH"] = "1"
    if terminal:
        master, slave = pty.openpty()
        p = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=slave, stderr=subprocess.PIPE, env=env)
        os.close(slave)
        out = Reader(master)
    else:
        master = None
        p = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        out = Reader(p.stdout.fileno())
    try:
        early = out.until(b"ready")
        still = p.poll() is None
        # Lets the program go on (also after a failure, so that it ends): its open of the FIFO has a writer now.
        gate = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK) if still else None
        if early is None or not still:
            problems.append(f"{name}: 'ready' was not written while the program waited (seen {out.seen!r}, "
                            f"running: {still})")
            return
        os.write(gate, b"x")
        os.close(gate)
        if out.until(b"1") is None or p.wait(DEADLINE) != 0:
            problems.append(f"{name}: the program did not finish after the FIFO was written (seen {out.seen!r})")
    except OSError as e:
        problems.append(f"{name}: {e}")
    finally:
        p.kill()
        p.wait()
        if master is not None:
            os.close(master)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    if os.name != "posix":
        print("console_input_check: skipped (select on pipes, FIFOs and ptys are POSIX's)")
        return 0
    problems = []
    cases = 0
    with tempfile.TemporaryDirectory(prefix="ceesafe-console-") as work:
        programs = {}
        for stem, source in (("dialogue", DIALOGUE), ("waiting", WAITING), ("discarded", DISCARDED)):
            path = os.path.join(work, stem + ".cee")
            with open(path, "w", encoding="utf-8") as f:
                f.write(source)
            exe = os.path.join(work, stem + (".exe" if os.name == "nt" else ""))
            b = subprocess.run([compiler, "build", path, "-o", exe], capture_output=True, text=True)
            if b.returncode != 0:
                problems.append(f"{stem}: build failed: {b.stderr.strip()[:400]}")
                continue
            programs[stem] = {"native": [exe], "interp": [compiler, "run", "--interp", path]}
        for mode, argv in programs.get("dialogue", {}).items():
            dialogue(argv, problems, f"dialogue ({mode})")
            cases += 1
        for mode, argv in programs.get("waiting", {}).items():
            waiting(argv, problems, f"flush through a pipe ({mode})", work, True, False)
            waiting(argv, problems, f"a line to a terminal ({mode})", work, False, True)
            cases += 2
        for mode, argv in programs.get("discarded", {}).items():
            p = subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                               timeout=DEADLINE)
            if p.returncode != 0 or p.stderr != b"written":
                problems.append(f"output to /dev/null ({mode}): expected 'written', exit 0: {p.returncode} "
                                f"{p.stderr[:300]!r}")
            cases += 1
    for problem in problems:
        print(f"FAIL {problem}")
    print(f"console_input_check: {cases} cases: {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
