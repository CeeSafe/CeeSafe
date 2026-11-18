# ADR 0173 — Handing a child what it inherits: sockets and shared memory

- Status: **Proposed**, 2026-11-18. Written at the owner's request ("Write the descriptor passing ADR"): it is the
  second half of ADR 0169 §5 question 2, which ADR 0172 §6 question 2 left as "the next ADR". Not accepted: §7 has
  the questions. Not implemented.
- Scope: lib/std (`Handover`, `Inherited`, `Process.spawnWith`, `Process.inherited`, `SharedMemory.anonymous`),
  the hosted runtime (`runtime/hosted/ceesafe_rt_io.c`, `ceesafe_rt_async.c`: POSIX, Windows), the interpreter's
  host services; type-system.md §26 (a new §26.18, and §26.16 and §26.17 where they are touched), `spec/api/std.md`,
  the guide's processes page. No change to the language or to the backends.

## 1. Context

- **What a worker needs from its supervisor.** httpserver's workers are processes that accept from one listening
  socket their supervisor bound, and count and publish in memory they all map. Today they have both because each
  is a copy of the supervisor: `fork`, declared through `extern "C"`.
- **What 0.70.0 and 0.71.0 gave.** `Process.spawn` (ADR 0169) starts a new program, which shares nothing with its
  parent but the environment, the working directory and the three standard streams. `SharedMemory` (ADR 0172) lets
  the new program find memory by a name it is told.
- **What is still missing.**
  - **The socket.** A spawned worker cannot be given the supervisor's listener. On Linux it can bind the port
    itself (`reusePort`, and the kernel spreads connections); on macOS and Windows it cannot: there the processes
    must share one socket. So the supervisor still forks on macOS and does not exist on Windows.
  - **Memory without a name.** A supervisor's memory is for its workers alone, and a name is something to choose,
    to leak when the supervisor is killed (POSIX), and for another process of the same user to open. httpserver
    makes its memory under a name and removes the name at once, which works only because its workers are forked.
- **What every system has for it.** A child can be started with chosen descriptors of its parent open in it
  (`posix_spawn`'s file actions; Windows' handle list, and `WSADuplicateSocket` for sockets). That is the mechanism
  systemd's socket activation and every prefork server use; it needs no name and leaves nothing behind.
- **What ADR 0169 left open on the way.** Its §2.2 asked that every descriptor the runtime opens be close-on-exec,
  so that a child gets nothing by accident; its notes say that was not audited. An explicit list of what a child
  gets is only worth having if nothing else gets through.

## 2. Options

| | the child is given | cost |
|---|---|---|
| **A. A list at spawn** (this ADR) | the listed sockets and memory, open in it from its first instruction | `Process.spawn` grows a second form; three systems' mechanisms |
| B. Sent over a Unix socket (`SCM_RIGHTS`) | any descriptor, at any time, also to a process that is not a child | a socket between the two, a protocol on it, async Unix sockets in lib/std; Windows has no equivalent for sockets but `WSADuplicateSocket` |
| C. Nothing: each worker binds, memory by name | | macOS and Windows cannot spread connections over processes that each bind; names (§1) |

- B is more general and much larger, and what it adds (a descriptor sent later, or to a stranger) no program here
  has asked for. It can be added beside A.
- C is what 0.71.0 allows, and is why this ADR exists.

## 3. Proposed decision

### 3.1 The API (lib/std)

```ceesafe
// The parent.
var handover = Handover.create();
let socket = handover.listener(borrow listener)?;      // u32: its place in the list, 0 for the first
let counters = handover.memory(borrow shared)?;        // 1
let child = Process.spawnWith(me.view(), arguments, ChildOptions.inherit(), borrow handover)?;

// The child, at its start.
var given = Process.inherited();                       // Inherited: what the parent handed over, or nothing
let listener = given.listener(0)?;                     // AsyncTcpListener: the same socket as the parent's
let shared = given.memory(1)?;                         // SharedMemory: the same memory
```

- **`Handover`** is a list of things the child is to have: `listener(borrow AsyncTcpListener)`,
  `datagrams(borrow AsyncUdpSocket)` and `memory(borrow SharedMemory)`, each returning the place it was given.
  It borrows what it lists, so the parent still has each one after the spawn, and one `Handover` serves every
  child that is to get the same things (a worker started again gets them as the first did). At most 16 places.
- **`Process.spawnWith(program, arguments, options, handover)`** is `Process.spawn` with the list. `spawn` is
  unchanged and hands over nothing.
- **`Process.inherited()`** gives the child's `Inherited`: `count()`, `kind(place)` (`Listener`, `Datagrams`,
  `Memory`, or null for a place that is empty or was taken), and `listener(place)`, `datagrams(place)` and
  `memory(place)`, which move the thing out: each place is taken once, and asking again, or for the wrong kind, is
  `Err(IoError.NotFound)`. In a program that was not started with a handover it is empty.
- **What is shared is the thing itself, not a copy.** Both processes accept from the one socket, and the kernel
  gives each connection to one of them; both map the one memory. Closing or dropping it in one process does not
  close it in the other.
- **`SharedMemory.anonymous(bytes)`**: memory with no name, for handing over. `create` and `open` are as they are.
- **Places are numbers**, in the order they were listed: the parent and the child are one program, or two that
  agree, as they must agree on what the memory holds.

### 3.2 What a child gets, and what it does not

- **Only what is listed.** Every other descriptor and handle of the parent is closed in the child. This ADR does
  the audit ADR 0169 §2.2 left: each place the runtime opens a descriptor (files, sockets, the reactor, pipes,
  directories, windows) is checked to be close-on-exec or non-inheritable, with a test that a child started by
  `Process.spawn` has no descriptor open but 0, 1 and 2 (§8).
- **Not to its own children.** What a child was handed is close-on-exec in the child from the moment it is taken,
  and what it never takes is closed when `Process.inherited()` is first called. A grandchild is told nothing: the
  word the runtime leaves for the child names its parent, and a process whose parent is another ignores it.
- **A child that is not a CeeSafe program** (or is an older one) has the descriptors open and does not know it. They
  are numbered from 3 in list order on POSIX, with `CEESAFE_HANDOVER` in its environment saying how many and of
  which kinds, so such a program can use them (`LISTEN_FDS` is the same idea; question 4 asks whether to speak
  that convention instead).

### 3.3 Systems

| | the list reaches the child by | a socket | memory |
|---|---|---|---|
| Linux, macOS, FreeBSD | `posix_spawn` file actions: each listed descriptor duplicated to 3, 4, … in the child, which clears close-on-exec there; `CEESAFE_HANDOVER` in the child's environment | the descriptor | the descriptor of the shared object: `SharedMemory` keeps it open for as long as the value lives (it closes it after mapping today), and `anonymous` is `memfd_create` on Linux, `shm_open` with a name removed at once elsewhere |
| Windows | the child is created suspended with an explicit handle list (`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`), told through a small inherited section what it was given, and resumed | `WSADuplicateSocketW` for the child's process id; the child makes its socket from the protocol information (`WSASocketW`). Handle inheritance alone is not reliable for sockets | an inheritable duplicate of the mapping's handle |
| Android, CosmoOS | `Unsupported` | | |
| the interpreter | `spawnWith` is `Unsupported`, as `spawn` is; `Process.inherited()` is empty | | |

- An allocation budget (ADR 0170) counts none of it.
- Freestanding: none of it (`profiles.md` §4).

## 4. What it does not do

- **No connected streams, files or pipes in the list.** A listener, a datagram socket and memory are what a
  supervisor's workers share for their whole life. A connection or a file handed to a child is a different
  contract (who reads what was already buffered? who closes?), and option B is its place.
- **Nothing sent after the start**, and nothing to a process that is not a child (option B).
- **No resource limits** for the child. httpserver's supervisor limits a worker's address space with `setrlimit`
  after `fork`; a spawned worker would set its own limit at its start, still through `extern "C"`. ADR 0169 §3
  left limits to their own design, and they stay there.
- **No fairness.** Which of several processes that accept from one socket gets a connection is the kernel's
  choice. httpserver's CI met a three-core macOS machine on which one of four such workers was given none for
  seconds. That is a property of the shared socket, with `fork` as with this.

## 5. Consequences

- **httpserver's supervisor can be written without `fork`**, on Linux, macOS and Windows: it binds the listener,
  makes anonymous memory, and starts itself again as each worker with both handed over
  (`Process.executable()`, an argument for the worker's number). `fork`, `waitpid` and `kill` go, for
  `ChildProcess`; `mmap` went with 0.71.0; `setrlimit` stays (§4).
- **A worker is a new program, not a copy.** It starts from `main`, so what the supervisor built before forking
  (routes, tables, parsed configuration) each worker builds for itself: slower to start, by the time `main`
  takes, and nothing is shared by accident. The cost is measured on httpserver's supervisor check before this
  lands (§8).
- **`SharedMemory` holds a descriptor** on POSIX for each value, where it held none: a program with many mappings
  uses that many descriptors.
- **`SharedMemory.anonymous`** replaces httpserver's create-then-remove, and with it a supervisor that is killed
  leaves nothing behind on any system.
- **The runtime's descriptors are all close-on-exec**, checked by a test, which also protects `Process.run` and
  `Process.capture`.

## 6. What it costs to get wrong

- A descriptor that leaks to a child is a socket that stays open after its owner closed it (a port that cannot be
  bound again, a peer that never sees the end). The test of §3.2 is what stands against it; it is in the contract
  suite on each system that has a runner.
- A place taken with the wrong kind is an error, not a socket treated as memory: the kinds travel with the list.

## 7. Questions for the owner

1. A list at spawn (option A), as `Handover`, `Process.spawnWith` and `Process.inherited()` (§3.1)? Recommended:
   yes.
2. The kinds: a TCP listener, a UDP socket and shared memory, and no connected streams, files or pipes (§4)?
   Recommended: yes. The UDP socket is there because an HTTP/3 server's workers share one as they share the
   listener; leaving it out would leave `fork` in exactly those servers.
3. `SharedMemory.anonymous(bytes)` in this ADR, with `SharedMemory` keeping its descriptor open on POSIX (§3.3)?
   Recommended: yes: memory to hand over should not need a name, and it cannot be handed over without the
   descriptor.
4. The runtime's own word to the child (`CEESAFE_HANDOVER`), not systemd's `LISTEN_FDS` and `LISTEN_PID`?
   Recommended: yes. `LISTEN_FDS` says sockets only and has no kinds, and a CeeSafe program started by systemd with
   real socket activation would then find its sockets through `Process.inherited()` only by accident of the same
   name. Reading `LISTEN_FDS` as a source of listeners can be its own small ADR.
5. Do the close-on-exec audit here (§3.2), not separately? Recommended: yes: the list means nothing without it.
6. Windows in the same release as POSIX, or after it? Recommended: the same release, written against §3.3 and
   compiled by the release workflow, and **said to be untested until something runs it**: there is still no
   Windows machine that runs ADR 0169's or 0172's checks, and that gap grows with each ADR. A Windows job that
   runs `child_check`, `shared_check` and this ADR's check is the first step of the implementation, not the last.

## 8. Implementation plan (once accepted)

1. A Windows job in the checks workflow that builds `ceesafec` and runs `tools/child_check.py` and
   `tools/shared_check.py` (question 6); what it finds in ADR 0169 and 0172 is fixed first.
2. The audit of §3.2 and its test: a program spawned by `Process.spawn` lists its open descriptors
   (`/proc/self/fd`, `/dev/fd`; `NtQuerySystemInformation` is not needed on Windows, where the handle list is the
   guarantee) and has only 0, 1 and 2.
3. Spec: §26.18; §26.16 (`spawnWith`), §26.17 (`anonymous`, the descriptor); `spec/api/std.md`; the guide;
   Appendix C.
4. Runtime and lib/std: `SharedMemory`'s descriptor and `anonymous`; the handover in `ceesafe_host_child_start`
   (a list of descriptors and kinds beside the streams); the child's side; the interpreter's answers.
5. Tests, in a check like `tools/shared_check.py`:
   - a parent binds a listener on a free port and makes anonymous memory, starts itself three times with both,
     and each child accepts connections and counts them in the memory: a client that makes 300 connections is
     answered 300 times, the counts sum to 300, and (Linux, where the kernel spreads them) every child answered
     some;
   - the parent drops its listener after the spawn and the children go on accepting; the children end and a new
     bind of the port succeeds (nothing leaked);
   - a place taken twice, a place of the other kind and a place past the end are `NotFound`; a program started
     without a handover has an empty `Inherited`; a grandchild started by `Process.spawn` has an empty one too;
   - a UDP socket handed to two children, each of which receives;
   - the interpreter: `Unsupported`, and empty.
6. httpserver's supervisor on it, with its checks, and the time to start four workers by `fork` and by spawn
   recorded in ROADMAP §10.
7. ROADMAP §10.
