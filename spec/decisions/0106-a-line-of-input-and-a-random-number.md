# ADR 0106 — A line of input and a random number

- Status: **Accepted** by the owner, 2026-11-12 ("Fix 1 and 2 as you suggested", then "We fix all gaps 1-6 first"),
  implemented on branch `impl/0106`; released in ceesafec 0.42.0. Raised while writing chapter 2 of the book, a number-guessing
  game, the first program a newcomer writes.
- Scope: two functions in `lib/std/host.cee` (`HostServices.readLine`, which the checker names `Console.readLine`,
  and `Random.below`); one C adapter and a buffer for standard input in `runtime/hosted/ceesafe_rt_io.c`; the
  interpreter's console (`src/interpreter/`); `spec/type-system.md` §26.3 and §26.12,
  `spec/intermediate-representation.md` §5.1, `spec/tooling.md` §6.3, `spec/testing.md`; the guide; a directive of
  the corpus runner. No change to the language, and no new diagnostic.

## 1. Context

The game asks for a guess, reads it, and says whether a secret number from 1 to 100 is smaller or bigger. With
ceesafec 0.40.0 the chapter met three gaps.

1. **No function reads a line.** `Console.read(mut buffer)` (§26.3) reads bytes. The chapter wrote the loop itself:
   a byte at a time into a `Vector<u8>`, one system call a byte, then `StringView.fromUtf8`.
2. **No random number in a range.** `Random.fill` (ADR 0060) gives bytes. The chapter filled four and took a
   remainder, which makes some numbers likelier than others (modulo bias).
3. **The game could not be played in the interpreter.** `ceesafec run --interp` kept the program's standard output
   until the run ended (or 64 KiB were kept; `src/interpreter/interpreter.cee`, `writeOutput`), flushing it only
   before a write to standard error, so that the two stay in order (`intermediate-representation.md` §5.1). Every
   prompt appeared after all the answers had been typed. `Console.flush()` did not help: it moved the bytes to the
   interpreter's own C-library buffer, which a pipe does not see until it fills.

What was there, and is kept:

- `Console.read` is a built-in the backend emits as `ceesafe_fd_read(0, …)`, one `read` of the descriptor;
  `Console.flush` (ADR 0077) is lib/std's `HostServices.flush`, which the checker reaches by name
  (`checkHostService`), as `Process.capture` and `File.kind` are. Both answer `IoError`. All of `Console` is hosted
  only (N0293 in the freestanding profile), and so is lib/std.
- A built program's standard output is the C library's `stdout`: written at each line to a terminal, in blocks to a
  pipe. A prompt that ends no line (`Console.write("Your guess: ")`) therefore stayed in the buffer while the
  program waited in `read`, natively too.
- `AsyncConsole` (lib/std) writes only; `Console` blocks its thread and is refused in async code (N0295).
- The interpreter's `Random.fill` is a fixed SplitMix64 sequence (§26.12), so its runs are reproducible.
  `ceesafec test --interp` and the corpus runner read the interpreter's output through pipes, after it ends.

## 2. Decision

### 2.1 `Console.readLine`

```ceesafe
Console.readLine(mut line)      // line: borrow mut String; the result is a Result<bool, IoError>
```

```ceesafe
var line = String.create();
while (Console.readLine(mut line)!) {
    let guess = line.view().trim();
    …
}
```

- It reads the next line of standard input into `line`, replacing what `line` held, without the line's ending:
  `\n`, or `\r\n`. A `\r` elsewhere stays. `Ok(true)` with a line, which may be empty; `Ok(false)` at the end of
  the input, with `line` empty. The last line of the input needs no ending.
- **A `String`, not bytes**, since what a program does with a line is text. A line that is not UTF-8 is
  `Err(InvalidInput)`, with `line` empty; the line is consumed, so the next call reads the one after it, and a
  program that wants to go on can. A program that wants the bytes has `Console.read`.
- Allocation failure is `Err(OutOfMemory)`, a failed read its `IoError`: `IoError` has both, so no new error type.
  Nothing is swallowed. `line` keeps its capacity, so a loop allocates only while lines grow.
- **One caller-owned `String`, a `bool` back.** Returning `Result<String?, IoError>` would allocate a string a
  line, and make the loop a `match` in a `loop`; the form chosen is a `while`.
- It is `HostServices.readLine` in lib/std, written in CeeSafe: the line's assembly from pieces, the ending's
  removal and the UTF-8 check. The checker names it `Console.readLine` as it names `Console.flush`
  (`src/check/expr.cee`). Hosted only, as `Console` is; the kernel and freestanding profiles have no standard input.

**One buffer in the runtime.** Reading a line needs to read ahead, and what is read ahead must not be lost to
`Console.read`. So the buffer is the runtime's, not the library's (`runtime/hosted/ceesafe_rt_io.c`):

- `int64_t ceesafe_host_console_read_line(uint8_t *out, uint64_t capacity)` copies bytes of standard input to
  `out` up to and including the first line feed, or `capacity` bytes if none comes before; it returns the count, 0
  at the end of the input, or a negated errno with nothing copied. It refills a 4,096-byte buffer with one `read`
  when the buffer is empty. Contract: it writes only `out[0..capacity]`, during the call; `capacity` is at least 1
  (`EINVAL` otherwise); the bytes are as read, in no encoding; nothing is allocated. lib/std calls it with a
  4,096-byte chunk on its stack, and gathers a longer line in a `Vector<u8>`.
- `ceesafe_fd_read` on descriptor 0, which is what `Console.read` is, takes what the buffer holds before it reads
  the descriptor. A program that mixes the two reads every byte once, in order. The emitted C does not change.
- One thread uses the buffer at a time, by a compare-and-swap flag: the runtime's mutexes are linked only into
  programs that use threads. A thread that finds the buffer in use does not wait, since its holder may be waiting
  for input: it reads the descriptor itself, `readLine` a byte at a time so that it takes nothing past its line.
  No byte is lost or read twice; which of two threads reading at once gets which bytes was never defined.
- Windows: the same code over the C runtime's `_read`. Standard input stays in text mode there, as it was for
  `Console.read`, so `\r\n` already arrives as `\n`.

**Standard output is flushed before standard input is read** from the system, by `readLine` and by `Console.read`
(`fflush(stdout)` before the `read`; nothing when the output's buffer is empty). A prompt is then seen before the
program waits, at a terminal and through a pipe, without `Console.flush()`. This is what C's `stdin` does on most
systems and what the chapter's reader expects. It changes when bytes are written, not which.

### 2.2 `Random.below`

```ceesafe
Random.below(n)                 // n: u64; the result is a Result<u64, IoError>
let secret = Random.below(100)! + 1;     // from 1 to 100
```

- An integer `r` with `0 <= r < n`, each of the `n` values equally likely.
- `n == 0` is `Err(InvalidInput)`: no integer is below 0. Not a panic: the function answers a `Result` already.
- Rejection sampling over `Random.fill`, in CeeSafe (`lib/std/host.cee`): 8 bytes, read as a `u64` low byte first;
  drawn again while the value is below `2^64 mod n` (computed as `(0 - n) mod n`); the result is the value modulo
  `n`. The values kept are a whole number of ranges of `n`. A draw is refused with probability below 1/2 whatever
  `n`, and never for a power of two. Nothing is allocated; the error is `fill`'s.
- In the interpreter it follows `fill`'s fixed sequence, one 64-bit word a draw: the same numbers every run, so a
  test of a game is reproducible there (the chapter's secret is 18).
- **One function.** `below(n)` rather than a pair of bounds: with two bounds the reader must know whether the upper
  one is included, an empty or reversed range is a second error case, and the common use is an index
  (`Random.below(v.length as u64)`). A signed or generic form is not included.

### 2.3 The interpreter's output

`ceesafec run --interp` writes the program's standard output as the program goes
(`intermediate-representation.md` §5.1):

- a line is handed to the interpreter's own standard output when it ends (or at 64 KiB without one). That output
  is the C library's: a terminal shows each line at once, a pipe or a file gets blocks, as from a built program;
- everything written so far is written out, whatever the output is, before the interpreter reads standard input
  for `Console.read` or `Console.readLine`, and at `Console.flush()`;
- before a write to standard error, before a child process runs, and at the end, as before.

So the same bytes reach each stream in the same order; only their time changes. With the output captured
(`ceesafe_run`), nothing changes. The interpreter's sources may not call `Console.flush` (ceesafec 0.8.0 builds
them): they flush with an empty `Console.writeError("")`, which flushes standard output first, as
`src/host/package_check.cee` already does.

The interpreter has the same single buffer for standard input (`Interp.stdinHeld`), read 4,096 bytes at a time;
with a given input (`ceesafe_run`'s `stdin`) both functions read that.

**A defect found and fixed.** The sandbox (`ceesafe_run`) refused `Console.flush()` as host I/O ("refused: the
sandbox allows no files, sockets or processes"), with 0.40.0 too. It is no host I/O; in the sandbox it now does
nothing, and `Console.readLine` reads the given `stdin`.

## 3. Not included

- `AsyncConsole.readLine`: standard input has no readiness on every system (`AsyncConsole` only writes), and a
  blocking read belongs in `Async.blocking`.
- Binary mode for standard input on Windows; a way to ask whether standard input is a terminal.
- A random float, a shuffle, a seeded generator.

## 4. Compatibility

- `Console.readLine` and `Random.below` were errors before (N0200: "'Console' has no member 'readLine'"), so no program changes
  meaning.
- A built program's output is flushed before it reads standard input. A program that relied on its output staying
  buffered across a read could not exist: the buffer's size was never specified.
- `File` values are not affected, except one opened while descriptor 0 was closed, which then is descriptor 0 and
  reads as standard input does.
- The interpreter writes earlier than it did. `tools/run_corpus.py --interp`, `ceesafec test --interp` and the
  guide's checks read pipes to their end and see the same bytes.

## 5. Tests

- `tests/corpus/run-pass/console-read-line.cee`, natively and with `--interp`: lines ended `\n` and `\r\n`, empty
  lines, a `\r` inside a line, a line that is not UTF-8 and the line after it, a line of 5,200 bytes, three bytes
  taken by `Console.read` between two lines, a last line without an ending, the end of the input twice.
- `tests/corpus/run-pass/random-below.cee`, both ways: `below(0)` is `InvalidInput`; `below(1)` is 0; 10,000 draws
  below 100 stay below it and take every value; below 3, each value 2,000 times of 6,000 within 450; below
  `3 * 2^62`, where a plain remainder would give the low third half of the draws, it gets a third; the largest `n`.
- `tests/corpus/check-fail/host-services.cee`: the argument that is not a `String`, the missing `mut`, the result's
  type, `below`'s result. Its comments were aligned by `ceesafec fmt`.
- `tools/run_corpus.py` gains `// stdin: text` (`testing.md` §3): the corpus had no way to give a program input.
  A test without the directive runs as before.
- `tools/console_input_check.py`, in the contract suite (POSIX): a dialogue through pipes, built and with
  `run --interp`, reading each prompt before writing its answer, one answer in two pieces; `Console.flush()`
  through a pipe and a finished line to a pty while the program waits to open a FIFO. With ceesafec 0.40.0 its
  interpreter cases fail.
- Three HIR snapshots (`check-pass/async-timeout`, `effects-of`, `string-braces-warning`) number their closures
  four higher, since lib/std has four more functions: reviewed, every changed line differs only in those numbers.
  `spec/api/std.md` gains the two functions.
- Run 2026-11-12, macOS arm64, on the branch, with the compiler built by itself (0.40.0 built the first, which
  built the second, which built the third; the second and third emit identical C):
  - `tools/run_corpus.py --compiler out/ceesafec --corpus tests/reference/tests/corpus`: 834 files, 0 failed (4
    freestanding partial, as before);
  - the same with `--interp --family run-pass --family run-fail`: 381 passed, 1 failed,
    `run-pass/async-send-file-reset`, which times out in a whole `--interp` run on this machine with ceesafec
    0.40.0 too and passes alone with both: not this change's;
  - `tools/console_input_check.py`: 6 cases, 0 problems; `tools/random_check.py`, `tools/test_command_check.py`,
    `tools/mcp_api_check.py`, `tools/freestanding_check.py` (host and 2 triples): 0 problems;
  - `tools/guide_check.py`: 158 examples, 0 problems; `tools/spec_check.py`: 0 problems with
    `src/library/documents.cee` regenerated (the branch does not commit that file);
  - the chapter's game played through pipes, built and in the interpreter, each prompt read before its answer.
- Not run on the branch: the whole `tools/contract_tests.py`, and anything on Linux or Windows. The Windows path of
  the runtime's change is the same code over `_read`, not compiled here.
