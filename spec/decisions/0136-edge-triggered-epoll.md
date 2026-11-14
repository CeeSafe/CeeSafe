# ADR 0136 — epoll edge-triggered, registered once, with a ready state per direction

- Status: **Accepted** by the owner, 2026-11-14 ("Yes, fix F11 and F12": cee-proxy's
  `~/dev/CeeSafeApps/cee-proxy/CEESAFE_FINDINGS.md` F12, which proposed this design). Implemented on branch
  `fix/cee-proxy-f11-f12`.
- Scope: `runtime/hosted/ceesafe_rt_async.c` (the Linux reactor, and lib/std's read, send and accept steps),
  `runtime/hosted/ceesafe_rt_io.c` (close epochs), `runtime/include/ceesafe/rt.h` (three functions), c-backend.md §6.
  No language, library or emitter change. kqueue, `poll()`, io_uring and Windows are unchanged.

## 1. Context

- **One-shot arming.** Since L1 A4b, a task that parks on a descriptor arms epoll for its direction with
  `EPOLLONESHOT` through `EPOLL_CTL_MOD` (or `ADD`). An event disarms it. So every wait costs one `epoll_ctl`, and no
  state outlives a wait, which the design chose so that a descriptor closed and reopened under the same number needs
  nothing (c-backend.md §6, before this ADR).
- **A step tries first.** lib/std's read, send and accept steps (`ceesafe_async_op_read`, `_send`, `_accept`; ADR 0035)
  attempt the system call and park only on `EAGAIN`. A server reads the upstream right after sending it a request, and
  reads the client right after answering it. The data is usually not there yet, so both reads fail before the wait.
- **Measured in cee-proxy** (`perf trace -s`, 5 s at a fixed 40,000 requests per second, 2 workers, same box and CPUs as
  nginx; Ubuntu 24.04, Xeon E-2236). Per proxied request:

  | | reads | writes | `epoll_ctl` | `epoll_wait` | other | total |
  |---|---|---|---|---|---|---|
  | nginx `proxy_pass` | 2.0 | 2.0 | 0.00 | 0.21 | 1.0 `getsockopt` | 5.2 |
  | cee-proxy, one-shot | 3.3 | 2.0 | 1.3 | 0.18 | 0 | 6.8 |

  With frame zeroing gone (cee-proxy's F11, fixed on the same branch), the kernel is 55 % of cee-proxy's cycles.

## 2. Decision

On Linux, epoll is edge-triggered by default.

1. **Registered once.** A descriptor is registered at its first park with `EPOLLIN | EPOLLRDHUP | EPOLLET`, and
   `EPOLLOUT` is added with `MOD` the first time a writer waits. It stays registered until it is closed: an event
   disarms nothing, and a task that leaves early takes nothing back.
2. **`ready` per direction**, in the descriptor's slot:
   - an event sets it, so an event with no task waiting is kept for the next attempt;
   - a park clears it, since a task parks only after an attempt that would block.
3. **No attempt that is known to block.** While a direction is registered and not ready, the read, send and accept
   steps park without the system call. On a stream socket (`SO_TYPE`, asked once at registration), a read that returns
   fewer bytes than asked took everything there was, so it clears `ready` as well, unless the slot is `eof`
   (`EPOLLRDHUP`, `EPOLLHUP` or `EPOLLERR`), since the end of stream is still to be read. A read that returns 0, or as
   many bytes as asked, leaves it set.
4. **Closed descriptors.** epoll forgets a descriptor when it is closed, but the slot would not. Every `close` in the
   runtime goes through `ceesafe_fd_close`, which first bumps the number's close epoch (`ceesafe_fd_epoch`: 4,096
   atomic counters, process-wide, shared by numbers 4,096 apart). A slot remembers the epoch it was registered under,
   and a park or a step that finds it moved registers again. A bump for another number in the bucket costs one
   registration, never a missed one.
5. **Registration polls.** `ADD` and `MOD` report readiness that predates them, so nothing that arrived between the
   failed attempt and the registration is lost.
6. **`CEESAFE_REACTOR_ONESHOT=1`** keeps the one-shot design, for diagnosis.

## 3. Why it is safe

- **A park follows an attempt that would block.** Every caller of `ceesafe_async_wait_readable` and `_writable` in the
  runtime and lib/std calls it after `EAGAIN` or `EINPROGRESS`, and lib/std has no public wait without an attempt.
  Events are taken only when no task runs, so none can arrive between a task's attempt and its park.
- **A short read** on a stream socket returns what the receive queue held. Data that arrives during or after the call
  makes a new edge, which the next `epoll_wait` reports, since the item is not on epoll's ready list once reported.
  Data and a FIN arriving together would leave the FIN unreported after the short read. That is what `eof` is for: an
  `EPOLLRDHUP` on the same event marks the slot, and the read is not taken as draining. Other descriptors (pipes,
  terminals, whose canonical mode returns one line at a time) never clear `ready` on a short read.
- **TLS**, in CeeSafe.Web, reads through lib/std's streams (OpenSSL memory BIOs), so the slot sees every read.
- **Executors.** A started task stays on its worker (ADR 0038), so a wait never crosses executors. A descriptor that
  two executors wait on, at different times, is registered in both epoll instances. Its edges reach both. That costs
  one spurious event at most, never a missed one.

## 4. Alternatives considered

- **Keep one-shot, and only skip the doomed read.** That still costs one `epoll_ctl` per wait.
- **Level-triggered, registered once.** An unread descriptor would wake every `epoll_wait`, unless it were disarmed,
  which is `epoll_ctl` again.
- **Deregister in each socket's drop instead of close epochs** (mio's design). The slot table is per executor, and a
  descriptor may be closed on another thread, so a drop could not reach the slot that needs it.
- **io_uring by default** (ADR 0035). M9 measured it as not yet faster, and it stays opt-in.

## 5. Not in this ADR

- kqueue: one-shot arming is already free, since changes go with the next `kevent` wait. The read before the wait is
  still made there.
- Many tasks accepting on one listener are all woken by one connection, as before (cee-proxy's F9 concerns their cost).
- Raw `AsyncSockets.read` and `send` still try first.

## 6. Testing (2026-11-14)

Run, not proposed:
- **macOS arm64** (kqueue, so the new code builds but stays off): `contract_tests.py` with `out/ceesafec` built from the
  branch. 45 ok, 2 Linux-only skipped, and `book` with 1 problem that `main` already has (Appendix J's 0.45.0 row
  does not name ADR 0135).
- **Linux x86-64** (Ubuntu 24.04, gcc 13.3, the `cee-bench` container on the owner's host binfadel), with ceesafec
  built there from the branch's emitted C:
  - the whole corpus with `--check-leaks`, edge-triggered: every family passes, except `emit-c/async-frame`, whose
    snapshot changed on the Mac after the copy (F11's layout);
  - the async programs one-shot (`CEESAFE_REACTOR_ONESHOT=1`), under `CEESAFE_REACTOR_POLL=1`, and under
    `CEESAFE_REACTOR_URING=1`: 55 of 55 each;
  - `run fortify` (gcc `-O2 -D_FORTIFY_SOURCE=2 -Wall -Werror`): run-pass, run-fail and ffi pass;
  - the async programs under gcc's AddressSanitizer and UndefinedBehaviorSanitizer: 55 of 55;
  - the async programs under ThreadSanitizer, with address randomisation off (`setarch -R`, since gcc 13's TSan
    refuses the host's `vm.mmap_rnd_bits=32` either way): 55 of 55, and the `share` programs 4 of 4;
  - `cancel_sweep.py`: 59 programs, 1,608 cancellation points, 0 fail;
  - `console_input_check.py`: 8 of 8;
  - `signal_check.py`: 20 of 20.
- **cee-proxy's conformance suite** (118 tests, WebSocket, retries, timeouts and drains): 118 of 118 on macOS with the
  branch's compiler.

## 7. Evidence (cee-proxy on binfadel, 2026-11-14)

- 2 workers on CPUs 1–2, `wrk -c100`, an nginx upstream returning 0 B. The four builds alternate, three rounds of
  15 s each:

  | Build | req/s | p99 | RSS |
  |---|---|---|---|
  | ceesafec 0.44.1 | 80.1k – 81.5k | 2.6 – 3.6 ms | 155 MB |
  | this branch, `CEESAFE_REACTOR_ONESHOT=1` (F11's frame fix only) | 114.1k – 115.5k | 1.8 – 1.9 ms | 39 MB |
  | this branch | 114.2k – 115.8k | 1.2 – 1.6 ms | 39 MB |

- **System calls per request** at a fixed 40,000 requests per second (`perf trace -s`, 5 s):

  | | reads | sends | `epoll_ctl` | `epoll_wait` | total |
  |---|---|---|---|---|---|
  | one-shot | 3.30 | 2.00 | 1.31 | 0.17 | 6.8 |
  | this ADR | 1.99 | 2.00 | 0.00 | 0.28 | 4.3 |

  nginx `proxy_pass` on the same box makes 5.2: 2 reads, 2 `writev`, 1 `getsockopt`.
- **Reading the result.** The calls fell by 37 %, but throughput at saturation rose about 1 % and p99 fell by about a
  quarter. CPU time per request at a fixed rate varied by ±12 % between runs of the same build, under the host's
  `powersave` governor, so this ADR claims nothing about it.
