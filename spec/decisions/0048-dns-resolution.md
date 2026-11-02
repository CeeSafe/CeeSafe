# ADR 0048 — DNS resolution for tasks

- Status: **Accepted** by the owner, 2026-11-02 ("Yes, I accept both ADRs with your recommendations"): every
  question of §5 as recommended. Asked for by the owner for CeeSafe.Web
  (`~/dev/CeeSafe.Web/docs/ceesafe-upstream-work.md`, item U5, phase A). CeeSafe.Web's HTTP client (its M5) connects to
  host names. Phase B, a resolver over UDP, is CeeSafe.Web's own work later, on U6.
- Scope: lib/std (`AsyncDns`), the hosted runtime (a resolver on worker threads), the interpreter's model.

## 1. Context

`AsyncTcpStream.connect` takes an address, not a name. A task that has a name has nowhere to turn it into addresses.

The system's resolver is `getaddrinfo`, which blocks and cannot be cancelled. `Async.blocking` runs blocking work on a
worker thread, but a cancelled task still waits for its worker (§40.4). A lookup that hangs for 30 seconds would hold
a cancelled request, and a deadline (`Async.timeout`) could not cut it short. A lookup needs its own path: the task can
leave, and the worker finishes alone.

## 2. Options

**How errors are reported.**

| | A. `IoError` kinds that exist (proposed) | B. A new `DnsError` |
|---|---|---|
| What | `AsyncError.Io(NotFound)` for an unknown name, `TimedOut` for a temporary failure, and so on | `Result<…, DnsError>`, with its own variants |
| Cost | the resolver's exact code is lost beyond those kinds | a second error type beside `AsyncError`: every caller converts, since `?` does not |

**A is proposed.** A caller wants to know "no such name" and "try again", and both are there. `IoError` gains nothing,
so no `match` breaks.

## 3. Proposed decision

### 3.1 The operation (lib/std, `net.cee`)

```ceesafe
public struct AsyncDns {
    public async fn resolve(host: StringView, port: u16) -> Result<Vector<SocketAddress>, AsyncError>;
}
```

- **Result.** The addresses of both families, each with `port`, in the resolver's order, duplicates removed. The
  system's settings (`/etc/hosts`, `gai.conf`, Windows' prefix policy) decide that order. It is never empty: no
  addresses is `NotFound`.
- **Literals.** A host that is an IPv4 or IPv6 literal (`SocketAddresses.parseIp`, zones included) is returned at once
  as its one address, with no thread.
- **Calls.**
  - POSIX: `getaddrinfo(host, NULL, {AF_UNSPEC, SOCK_STREAM, flags 0})`. `AI_ADDRCONFIG` is off, so `localhost`
    resolves on a host without IPv6.
  - Windows: `GetAddrInfoW`, the name converted from UTF-8 to UTF-16.
  - A name is passed as it is given: an internationalised name must already be in its ASCII form (punycode).
- **Errors** (`AsyncError.Io(kind)`):

  | Resolver result | Kind |
  |---|---|
  | `EAI_NONAME`, `EAI_NODATA` (`WSAHOST_NOT_FOUND`, `WSANO_DATA`), or no addresses | `NotFound` |
  | `EAI_AGAIN` (`WSATRY_AGAIN`) | `TimedOut` (temporary: try again) |
  | `EAI_MEMORY` | `OutOfMemory` |
  | `EAI_FAMILY`, `EAI_SERVICE`, `EAI_BADFLAGS`; a name that is empty, has a NUL, or is longer than 253 bytes | `InvalidInput` |
  | `EAI_SYSTEM` | the kind of its `errno` |
  | anything else (`EAI_FAIL`, `WSANO_RECOVERY`) | `Other(EIO)` |

- **Profiles.** CosmoOS userland has no resolver yet: its lookups of a name return `Unsupported` (literals still work).
  The operation is hosted only, like the sockets.

### 3.2 Workers and cancellation

- **The pool.** Lookups run on at most **4** resolver threads per process, started when first needed and kept until it
  exits. Further lookups wait in first-come order. A flood of lookups costs a queue entry each, not a thread each.
- **Waiting.** The task parks until its lookup is done; the worker wakes it through its executor, as `Async.blocking`'s
  worker does.
- **Cancellation.**
  - A cancelled task returns `Err(Cancelled)` at once, whether its lookup is queued (taken out) or running (abandoned).
  - The lookup's record is allocated by the runtime, not in the task's frame. An abandoned one is freed by its worker
    when `getaddrinfo` returns, so nothing refers to the frame after the task leaves.
  - So `Async.timeout(2000, …, async () => await AsyncDns.resolve(…))` returns after 2 seconds, whatever the resolver
    does.
- **Allocation.**
  - If the record cannot be allocated: `OutOfMemory`.
  - If no thread can start: the lookup runs on the calling thread, as `Async.blocking` does when it cannot start one.
    The result is the same, but it is not cancellable then.

### 3.3 The runtime (rt.h)

```c
void *ceesafe_dns_start(const uint8_t *host, uint64_t length, uint16_t port); /* NULL: out of memory */
int32_t ceesafe_dns_poll(void *lookup);   /* 0 not done (parks), 1 done, else a negated errno (its kind) */
uint32_t ceesafe_dns_count(void *lookup);
void ceesafe_dns_address(void *lookup, uint32_t index, uint8_t *bytes, uint32_t *family, uint32_t *scope);
void ceesafe_dns_end(void *lookup);       /* done: frees it; still queued: unqueued and freed; running: abandoned */
```

lib/std reads the addresses into a `Vector<SocketAddress>`, then ends the lookup, in a `drop` too, so every path frees it
once.

### 3.4 The interpreter

The interpreter runs on stage 0's library (0.3.2), which has no resolver. Its model therefore resolves:
- literals, as natively;
- `localhost`, as `127.0.0.1` then `::1`.

Any other name is `Err(Io(Unsupported))`. A test can rely only on literals and `localhost`, and gets the same answer
everywhere: the interpreter's `localhost` order is fixed, so tests should not depend on it natively.

## 4. Consequences

- CeeSafe.Web's client resolves, connects to the addresses in order, and can bound the whole lookup with a deadline.
- A cancelled lookup's thread stays busy until the resolver returns. With 4 threads, at most 4 hung lookups can delay
  the queue. That is the price of `getaddrinfo`, until phase B.
- New diagnostics: none.

## 5. Questions for the owner

1. Report errors as existing `IoError` kinds through `AsyncError.Io` (option A), not a new `DnsError`? Recommended: yes.
2. At most 4 resolver threads per process, queueing beyond? Recommended: yes.
3. A cancelled lookup returns at once and is abandoned to its worker, which frees it? Recommended: yes.
4. The interpreter resolves literals and `localhost` only, other names `Unsupported`? Recommended: yes.
5. Release U8 (ADR 0047) and U5 together as **0.8.0**? Recommended: yes. They add to the manifest and the library, so
   a minor version, not a patch.

## 6. Implementation plan (once accepted)

1. Spec: §40.4 (`AsyncDns`), c-backend.md §6 (the resolver), the API page.
2. Runtime: the lookup record, the pool and its queue, POSIX and Windows calls, the error mapping; then
   `tools/gen_runtime.py`.
3. lib/std: `AsyncDns.resolve`, a record type with a `drop`; then `tools/gen_library.py`.
4. The interpreter's model (§3.4).
5. Tests:
   - corpus run-pass, interpreter and native:
     - literals, IPv4 and IPv6 with a zone, each its one address;
     - `localhost`, compared as a set: it contains `127.0.0.1` or `::1`;
     - an invalid name (`NotFound`; the reserved `.invalid` top-level domain, RFC 6761);
     - an empty or over-long name (`InvalidInput`);
   - native only:
     - a lookup cancelled while running, under a resolver made slow by a test hook (`CEESAFE_DNS_DELAY_MS`); ASan shows
       the abandoned record freed with `live=0`;
     - 100 lookups at once, which never use more than 4 threads;
   - `tools/cancel_sweep.py` over the new tests; every reactor; Windows on the Windows 11 ARM64 test host.
6. The guide (`guide/async.md`, sockets), ROADMAP §10, the 0.8.0 release.
