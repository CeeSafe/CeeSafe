# ADR 0237 — The CPU count, decimal digits on a byte vector, and string literals as byte needles

- Status: **Proposed — implemented** 2026-11-24 on the branch `fix/findings4-ceeproxy`, at the owner's instruction
  ("Tackle CeeSafe for still open items": cee-proxy's `CEESAFE_FINDINGS.md` F8, R8 and R4). The names in §2 are this
  ADR's. §5's questions are the owner's.
- Scope: type-system.md §16.6, §23.2, §38.2; lib/std's `ThreadServices`; lib/core's `AppendDecimal` (new);
  `src/check/bytes_text.cee`, `src/check/threads.cee`; the hosted runtime's `ceesafe_thread_cpu_count`; the
  interpreter's model of it; the guide and the book's Appendix C. No syntax.

## 1. Context

cee-proxy starts one executor per CPU and found three gaps, each worked around there:

- **F8.** lib/std cannot say how many CPUs there are. cee-proxy parses `/sys/devices/system/cpu/online`, which ignores
  `taskset` and a container's cpuset, and answers 1 off Linux.
- **R8.** A `Vector<u8>` has `appendHex`, `appendOctal`, `appendFixed` and `appendPadded`, but no decimal. A `String` has
  `appendNumber` and a `ByteString` `appendNumber` and `appendUnsigned`. Four of cee-proxy's packages wrote it by hand.
- **R4.** `path.startsWith("/api")` on a `Slice<u8>` is N0205; `path.startsWith("/api".bytes())` is meant, and a `match`
  on the same slice takes the literal as its bytes already.

A fourth, R10 (`??` on an optional of a move type read through a reference), needs nothing: `(borrow r.key ?? borrow
empty).view()` compiles (ADR 0195's optional borrows), and N0251 on the bare form is right, since it would move.

## 2. Decision

### 2.1 `Thread.cpuCount() -> u32`

How many CPUs the process may run on, at least 1; lib/std's `ThreadServices.cpuCount`, hosted, beside `Thread.sleep`.

- Linux: the process's affinity mask (`sched_getaffinity` of the process's main thread, in a buffer that grows while
  the kernel refuses its size), which `taskset`, cgroup cpusets and `docker --cpuset-cpus` set. A thread that pinned
  itself does not change the answer. A CPU-time quota (`docker --cpus`) does not either: it bounds time, not CPUs.
- Windows: the process's affinity mask where the machine has one processor group, else all groups' active CPUs.
- Elsewhere, or when that fails: the online CPUs (`_SC_NPROCESSORS_ONLN`). CosmoOS: 1.
- The interpreter runs one thread at a time and answers 1.
- It has no effect: it reads the system's count and never waits, so async code may call it. It is asked each time.

### 2.2 `v.appendNumber(n: i64)` and `v.appendUnsigned(n: u64)` on a `Vector<u8>`

lib/core's interface `AppendDecimal`, implemented for `Vector<u8>`, with `ByteString`'s names and types:
`Result<void, AllocError>`, one growth at most, the vector unchanged on failure. A `String`'s built-in `appendNumber`
stays as it is.

### 2.3 A string literal as the needle of a byte slice's search

In `indexOf`, `lastIndexOf`, `contains`, `startsWith`, `endsWith`, `equalsIgnoreAsciiCase`, `splitOnce` and `split` on a
byte slice or a byte vector, an argument that is a string literal is its UTF-8 bytes, as `"…".bytes()` is. Only a
literal: a `StringView` value is still N0205 and is written `s.bytes()`, so no conversion happens that the source does
not show. Nothing that compiled changes.

## 3. Options not taken

- **`Process.cpuCount()`.** `Process` is about the program and its children; what uses the count starts threads.
- **A general conversion of `StringView` (or of literals) to `Slice<u8>`.** It would reach every call and overload the
  meaning of `"…"`; the searches are where byte code meets fixed text. A byte-string literal (`b"…"`, a reserved
  prefix) is a larger change that this does not prevent.
- **Counting a cgroup's CPU-time quota.** It can be fractional and changes while the program runs.

## 4. Tests

- `run-pass/adr0237-bytes-and-literals.cee`: the limits of `i64` and `u64`; every search with a literal, an empty one,
  one that is not ASCII, on a vector too, and a `Slice<u8>` needle as before.
- `run-pass/adr0237-cpu-count.cee`: at least 1, and the same twice.
- `check-fail/adr0237-literal-needles.cee`: a `StringView` needle is N0205.
- By hand on Linux: 2 in a container with `--cpuset-cpus 5,11`, 1 there under `taskset`; on the Mac `hw.logicalcpu`. The
  Windows branch was written, not compiled here.

## 5. Questions for the owner

1. The names: `Thread.cpuCount`, `AppendDecimal`.
2. Should `ByteStringView`'s searches take literals too? Not done: its needles are lib/core parameters, not the checker's.
