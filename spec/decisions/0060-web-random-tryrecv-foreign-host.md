# ADR 0060 — Random bytes, `Channel.tryRecv`, C allocations, `IoError.HostNotFound`

- Status: **Accepted** by the owner, 2026-11-03 ("Lets go back to CeeSafe once again to implement the following
  request by CeeSafe.Web"), with the choices of §3 as recommended; §5 lists them for the owner to revisit. Asked for by
  CeeSafe.Web (`~/dev/CeeSafe.Web/docs/ceesafe-upstream-work.md`, items U16, U13, U14 and U15), as one release.
- Scope: lib/std, the hosted and core runtime, the built-in `IoError`, the interpreter, the build's C headers.

## 1. Context

Four small gaps, each found while CeeSafe.Web designed a milestone:

- **U16, random bytes** (blocks the WebSocket client, Web ADR 0010 §3.6). lib/std gives a program no random bytes. The
  hosted runtime reads the system's source (`cee_random_fill`, `runtime/hosted/ceesafe_rt_io.c`) only to seed
  `HashMap`'s keys (`ceesafe_random_key`, ADR 0052). A WebSocket client needs 16 random bytes per connection
  (`Sec-WebSocket-Key`, RFC 6455 §4.1) and a masking key per frame "derived from a strong source of entropy" (§5.3);
  QUIC connection ids, session ids and CSRF tokens follow.
- **U13, a receive that does not wait** (Web ADR 0008 §3.3–3.4). `Channel<T>` (ADR 0032) has `trySend`, but every
  receive waits. `web_sse` wants to drop the oldest event of a full queue, and to write every event already queued
  before one flush.
- **U14, C allocations** (Web ADR 0002 §3.5, §6.1). OpenSSL can allocate through `CRYPTO_set_mem_functions`. Through
  `ceesafe_alloc` its allocations would be counted and fault-injected, so the allocation-failure sweep would reach
  OpenSSL's out-of-memory paths, but its global state (error strings, providers, the default context) lives until
  the process exits, and every test would report it as a leak. A package's C file has no declared header for the
  allocator either.
- **U15, a name that does not resolve** (Web M5). `AsyncDns.resolve` maps `EAI_NONAME`, `EAI_NODATA` and
  `EAI_ADDRFAMILY` to `-ENOENT` (ADR 0048 §3.1), so an unknown host is `IoError.NotFound`, whose message is "No such
  file or directory".

## 2. Options

**U16: where the bytes come from on each call.**

| | A. The system's source on every call (proposed) | B. A user-space ChaCha20 seeded from it |
|---|---|---|
| Cost per call | macOS and the BSDs: `arc4random_buf`, a user-space ChaCha20 already; Linux: one `getrandom` system call (the vDSO's `getrandom` where glibc has it); Windows: `BCryptGenRandom`, a user-mode per-processor generator | a few nanoseconds |
| Risks | none of its own | a generator state to reseed after `fork`, to keep per thread or under a lock, and to get right: what the system already does |

A per-frame call costs about as much as the frame's own socket write at worst (§7 measures it). B would duplicate the
system's generator and its fork handling; it can come later behind the same API if a measurement asks for it.

**U16: fallible or not.** `getrandom` can fail (`ENOSYS` before Linux 3.17, then `/dev/urandom` may be missing in a
container), and CosmoOS userland has no source at all. HashMap can keep a fixed key, since its order is its only
use of the bytes, but a masking key or a session token must never silently be predictable: **fallible**,
`Result<void, IoError>`.

**U13: how "closed" is told apart.** A `Result<T?, …>` makes every caller match three cases where the uses above
need two; `isClosed()` answers the rare question separately. **`tryRecv() -> T?` and `isClosed() -> bool`.**

**U14: an allocator number or a separate API.** The request names an allocator number; the core already routes
every allocation by one (`CEE_ALLOC_SYSTEM`, `CEE_ALLOC_FAILING`). A number keeps one counter of attempts, so
`CEESAFE_FAIL_ALLOC_AT` numbers CeeSafe's and C's allocations in one sequence, which is what the sweep needs.

**U15: a new variant or a better message.** A message for `NotFound` that fits both a file and a host cannot be glibc's
wording, and a program could still not tell the two apart. **A new variant**, after `Unsupported` so every earlier
variant keeps its tag (as ADR 0044 did).

## 3. Decision

**U16.**

```ceesafe
// lib/std, hosted
public struct Random {
    /// Fills `into` with bytes from the system's cryptographic random source.
    public fn fill(into: SliceMut<u8>) -> Result<void, IoError>;
}
```

- The runtime's `int64_t ceesafe_random_fill(uint8_t *out, uint64_t length)`, 0 or a negated errno:
  - Linux and other POSIX systems: `getrandom(out, n, 0)`, retried on `EINTR` and for short reads; on `ENOSYS`, or
    where the C library has no `getrandom`, `/dev/urandom`. `getrandom` without flags waits only until the kernel's
    pool is first initialised, early in boot.
  - macOS and the BSDs: `arc4random_buf`, which cannot fail.
  - Windows: `BCryptGenRandom(NULL, …, BCRYPT_USE_SYSTEM_PREFERRED_RNG)`, in pieces of at most `ULONG_MAX` bytes;
    a failure is `EIO`.
  - CosmoOS userland: `EOPNOTSUPP` (`IoError.Unsupported`).
- No effects (`@effects()`): it does not allocate, and after boot it does not wait, so async code and `drop` blocks
  may call it, once per WebSocket frame.
- `HashMap`'s seed reads through the same function; where it fails, the seed keeps its fixed key, as today.
- **The interpreter** answers a fixed sequence (SplitMix64 from a fixed seed, continuing from call to call), so a run
  stays reproducible (§40.8). Its bytes are not secret; the guide says so.
- Hosted only: in freestanding code `Random` is not defined (N0200, as every lib/std type).

**U13.**

```ceesafe
// lib/std, Channel<T>
public fn tryRecv(borrow this) -> T?;   // the next buffered value, or null at once (empty, or closed and empty)
public fn isClosed(borrow this) -> bool; // whether close() has been called; values may still be buffered
```

- Both have no effects, like `trySend`. A drained channel is `isClosed()` then `tryRecv() == null`, in that order:
  no value is sent after `close`, so the order rules out a value sent between the two calls.
- The runtime's `bool ceesafe_chan_closed(void *channel)` reads the flag under the channel's lock.

**U14.**

- `#define CEE_ALLOC_FOREIGN 2u`: an allocator number for C code. Its attempts are counted and fault-injected like
  every other (one sequence for `CEESAFE_FAIL_ALLOC_AT`); its live blocks are counted apart and are not a leak.
- With it, `ceesafe_alloc`, `ceesafe_realloc` and `ceesafe_free` follow C's `malloc`, `realloc` and `free` where C
  differs: `ceesafe_free(NULL, …)` does nothing, and `ceesafe_realloc(NULL, n, …)` allocates.
- A new header, `ceesafe/foreign.h`, declares the three functions and the number. ceesafec writes it beside
  `ceesafe/rt.h` in every build, whose include directory every C file of the build already has (`-I` on the work
  directory, ADR 0047 §3.3), so a package's C files include it as `#include "ceesafe/foreign.h"`. `rt.h` includes
  it, so the declarations exist once.
- The report (`CEESAFE_ALLOC_REPORT`, `ceesafec test`) keeps its line, `ceesafe alloc: attempts=<n> live=<m>`, where
  `attempts` now includes C's and `live` does not; when C allocated at all, a second line follows:
  `ceesafe alloc: foreign held=<k>`, the blocks C still holds at exit, for information.

**U15.**

- `IoError.HostNotFound`, variant 19, after `Unsupported`; its message is "Host not found".
- The resolver's "no such name" (`EAI_NONAME`, `EAI_NODATA`, `EAI_ADDRFAMILY`; Windows `WSAHOST_NOT_FOUND`,
  `WSANO_DATA`) and an answer without addresses are `HostNotFound`, through a runtime-only error number
  (`CEE_ERROR_HOST_NOT_FOUND`, 65536, which no system's errno uses) that `ceesafe_io_kind` maps to it.
- The interpreter's resolver model answers `HostNotFound` for the reserved `.invalid` top-level domain (RFC 6761
  §6.4), and `Unsupported` for other names besides `localhost`, as before.

## 4. Consequences

- CeeSafe.Web's WebSocket client gets its keys and masks; `web_sse` gets `DropOldest` and batched flushes; OpenSSL's
  allocations join the sweep in test builds; a DNS failure says so.
- **A source change:** a `match` over `IoError` that names every variant and has no `_` arm no longer covers it
  (N0256), as with ADR 0044 in 0.7.0. ceesafec's own sources have none.
- New diagnostics: none.

## 5. Choices for the owner to revisit

1. `Random.fill` reads the system's source on every call, with no user-space generator (§2, §7's measurements).
2. `Random.fill` is fallible, `Result<void, IoError>`; CosmoOS userland is `Unsupported`.
3. The interpreter's random bytes are a fixed, reproducible sequence.
4. `Channel.tryRecv() -> T?` with a separate `isClosed()`.
5. `CEE_ALLOC_FOREIGN`'s live blocks are reported on their own line and never fail a test.
6. `IoError.HostNotFound`, message "Host not found", also for a name with no addresses.

## 6. Implementation plan

1. Runtime: `ceesafe_random_fill`, `ceesafe_chan_closed`, `CEE_ALLOC_FOREIGN`, `CEE_IO_HOST_NOT_FOUND` and the
   resolver's mapping; `ceesafe/foreign.h`; `tools/gen_runtime.py`.
2. The built-in `IoError` (`src/check/structs.cee`) and its message (core runtime, interpreter).
3. lib/std: `Random`, `Channel.tryRecv` and `isClosed`, `HostServices.hostError`; `tools/gen_library.py`.
4. The interpreter's models: `ceesafe_random_fill`, `ceesafe_chan_closed`, the `.invalid` names, error 65536.
5. Tests: run-pass for each (natively and in the interpreter), a check-fail for the freestanding name, and a
   contract-suite tool for the native random bytes and for a C file allocating through `CEE_ALLOC_FOREIGN` under
   the sweep.
6. Spec (type-system.md §26.2, §26.12, §39 channels, §28.6; tooling.md §4.1), the guide, ROADMAP §10.

## 7. Implementation notes (2026-11-03)

- **The cost of `Random.fill`**, a 4-byte fill as the mean of 100 000 calls (`tools/random_check.py`): 0.04–0.05 µs on
  macOS (`arc4random_buf`), 0.57 µs on Debian 12 (Linux 6.1, glibc 2.36: a `getrandom` system call each time), and
  below the 15.6 ms tick of Windows' millisecond clock over the 100 000 calls, so under 0.16 µs, on Windows 11 ARM64.
  No user-space generator is needed (§2).
- **`rt.h` includes `"foreign.h"`**, its neighbour, not `"ceesafe/foreign.h"`: the first form compiled only where the
  build's directory was on the include path, and C written by `--emit c` and compiled by hand has no such flag (found
  on Debian).
- **Package C files already had the runtime's headers**: `-I` on the build directory reaches every C file, those
  compiled apart with `include-dirs` too (`src/host/toolchain.cee`, `pushCommonFlags`). The new header gives C code a
  small contract instead of all of `rt.h`.
- **`ceesafe_free(NULL, …)` does nothing** for every allocator, and `ceesafe_realloc(NULL, n, …)` allocates for every
  one; generated code never does either (its frees are guarded by a capacity or a null check), so only C sees the
  difference.
- **The interpreter's sources cannot name `HostNotFound`**, since stage 0 (ceesafec 0.3.2, Decision 0009) does not
  have it: `src/interpreter/io.cee` recognises the variant by its message, as it does ADR 0044's.
- **check-pass snapshots**: `async-timeout.hir` and `effects-of.hir` differ in closure ids only (lib/std's new items
  move them), checked normalised and updated.
