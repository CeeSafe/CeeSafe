# ADR 0061 — C allocations that are counted but never failed, for a library's one-time initialisation

- Status: **Accepted** by the owner, 2026-11-03 ("You can take over CeeSafe session and tackle U17 then release as per
  the worfklow v* tag"), with §3 as below. Asked for by CeeSafe.Web
  (`~/dev/CeeSafe.Web/docs/ceesafe-upstream-work.md`, item U17).
- Scope: the core runtime's allocator, `ceesafe/foreign.h`, the allocation report.

## 1. Context

ADR 0060 gave C code `CEE_ALLOC_FOREIGN`: its allocations are counted and fault-injected in one sequence with
CeeSafe's, and its blocks held at exit are reported apart, never as a leak. CeeSafe.Web routed OpenSSL's allocations
through it (`CRYPTO_set_mem_functions`), and its allocation sweep then failed at the first points of every TLS test.

Those points are OpenSSL's one-time initialisation, which cannot survive a failed allocation. In OpenSSL 3.6.4,
`CRYPTO_malloc`'s failure path raises an error, and raising an error calls `OPENSSL_init_crypto` again inside the
`pthread_once` that is running it:

```text
_os_once_gate_recursive_abort ← pthread_once ← OPENSSL_init_crypto ← ossl_err_get_state_int ← ERR_new
← CRYPTO_malloc ← CRYPTO_THREAD_lock_new ← ossl_init_base ← pthread_once ← OPENSSL_init_crypto ← SSL_CTX_new_ex
```

macOS aborts the recursion; on Linux a recursive `pthread_once` deadlocks. Every later allocation fails cleanly.
Other C libraries have the same shape: global setup run once, with no way back from a failure in it. A sweep that
fails those allocations measures the library's initialisation, which the program cannot handle, instead of the
program's own out-of-memory paths.

## 2. Options

| | A. A second allocator number (proposed) | B. Pause fault injection (`ceesafe_faults_hold()` / `release()`) |
|---|---|---|
| What C writes | its allocation functions choose the number while its initialisation runs | two calls around the initialisation |
| Threads | per call: nothing global changes | a process-wide switch: another thread's allocations would also escape the sweep while it is on |
| Sweep numbering | the initialisation's allocations are not attempts, so the sequence is the program's own | the same, if paused allocations are not counted |

A keeps the allocator's contract per call, as `CEE_ALLOC_FOREIGN` is, with no state to get wrong.

## 3. Decision

- `#define CEE_ALLOC_FOREIGN_INIT 3u` in `ceesafe/foreign.h`: an allocator number for a C library's one-time
  initialisation.
  - Its allocations are never failed, by `CEESAFE_FAIL_ALLOC_AT` or by `FailingAllocator`, and are not counted as
    attempts. So a sweep's numbering, and `attempts` in the report, are the program's own.
  - Its live blocks count with `CEE_ALLOC_FOREIGN`'s, in `foreign held=<k>`, never as a leak. A block may be
    reallocated or freed under either foreign number, since a library cannot tell which made it.
  - `ceesafe_alloc`, `ceesafe_realloc` and `ceesafe_free` follow `malloc`, `realloc` and `free` with it, as with
    `CEE_ALLOC_FOREIGN`.
- It is for initialisation only. A program that sends every allocation through it has opted its C library out of the
  sweep; the guide says so.

## 4. Consequences

- CeeSafe.Web's shim runs `OPENSSL_init_ssl` once under `CEE_ALLOC_FOREIGN_INIT` (its allocation functions check a
  flag set only during that call), then routes every other allocation through `CEE_ALLOC_FOREIGN`.
- No source change. No new diagnostics.

## 5. Implementation notes (2026-11-03)

- `alloc_refused` returns false at once for `CEE_ALLOC_FOREIGN_INIT`, before counting the attempt;
  `alloc_count_live` counts its blocks in `alloc_foreign`.
- `tools/foreign_alloc_check.py` covers the number:
  - the package's C file makes and keeps one block under `CEE_ALLOC_FOREIGN_INIT`, reallocates it under
    `CEE_ALLOC_FOREIGN`, and keeps it (`foreign held=2`);
  - over every `CEESAFE_FAIL_ALLOC_AT` point, the initialisation never fails;
  - `attempts` is the same with and without it.
