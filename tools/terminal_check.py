#!/usr/bin/env python3
"""Checks of `Console.isTerminal` (ADR 0221 §2.3) that the corpus cannot make, since the corpus gives every program
pipes: under a pseudo-terminal (Python's `pty`, POSIX only), a stream that is the terminal is `true` and one that is
redirected to a pipe or a file is `false`, natively and in the interpreter. Each case sets standard input, output
and error independently, and the program prints the three answers on one line.

usage: python3 tools/terminal_check.py --compiler out/ceesafec
"""

import argparse
import os
import subprocess
import sys
import tempfile

PROGRAM = """
fn main() {
    Console.write(Console.isTerminal(ConsoleStream.Input));
    Console.write(" ");
    Console.write(Console.isTerminal(ConsoleStream.Output));
    Console.write(" ");
    Console.writeLine(Console.isTerminal(ConsoleStream.Error));
}
"""

# (input, output, error): True for the terminal, False for a pipe (input: /dev/null, a file).
CASES = [
    (True, True, True),
    (True, False, True),
    (False, True, True),
    (True, True, False),
    (False, False, False),
]


def run_case(command, case):
    """The words the program printed, with each stream on the terminal where the case says so."""
    import pty
    master, slave = pty.openpty()
    try:
        stdin = slave if case[0] else subprocess.DEVNULL
        stdout = slave if case[1] else subprocess.PIPE
        stderr = slave if case[2] else subprocess.PIPE
        p = subprocess.Popen(command, stdin=stdin, stdout=stdout, stderr=stderr)
        out, _ = p.communicate(timeout=120)
        if case[1]:
            # Read what the terminal holds while this side is still open: macOS drops it once the last descriptor
            # of the other side closes.
            import select
            data = b""
            while select.select([master], [], [], 1.0)[0]:
                try:
                    chunk = os.read(master, 4096)
                except OSError:
                    break  # Linux: EIO once the terminal's other side is closed
                if not chunk:
                    break
                data += chunk
            out = data
        return out.decode(errors="replace").split()
    finally:
        if slave >= 0:
            os.close(slave)
        os.close(master)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    problems = []
    if os.name == "nt":
        print("terminal_check: SKIP (pseudo-terminals are POSIX only; Windows' answer is GetConsoleMode's)")
        return
    with tempfile.TemporaryDirectory(prefix="ceesafe-terminal-") as work:
        source = os.path.join(work, "terminal.cee")
        with open(source, "w") as f:
            f.write(PROGRAM)
        exe = os.path.join(work, "terminal")
        b = subprocess.run([compiler, "build", source, "-o", exe], capture_output=True, text=True)
        if b.returncode != 0:
            problems.append(f"build failed: {b.stderr.strip()[-400:]}")
        commands = [("natively", [exe])] if b.returncode == 0 else []
        commands.append(("in the interpreter", [compiler, "run", "--interp", source]))
        for where, command in commands:
            for case in CASES:
                want = ["true" if t else "false" for t in case]
                got = run_case(command, case)
                if got != want:
                    problems.append(f"{where}, terminal on {case}: {got}, want {want}")
    print(f"terminal_check: {len(CASES)} arrangements of the standard streams, natively and in the interpreter; "
          f"{len(problems)} problems")
    for problem in problems:
        print("  FAIL " + problem)
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
