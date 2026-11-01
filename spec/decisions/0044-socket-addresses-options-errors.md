# ADR 0044 — Socket addresses, IPv6, socket options and network error kinds

- Status: **Accepted** by the owner, 2026-11-02 ("Yes, I accept the ADR each with your recommendation"): every
  question of §5 as recommended. Asked for by the owner for CeeSafe.Web, a web framework built on CeeSafe
  (`~/dev/CeeSafe.Web/docs/ceesafe-upstream-work.md`, items U1 and U2). This is the first of that document's ADRs: this
  one is one API surface (addresses, options and the errors they report); vectored sends and the operation record's
  size (U3), `Async.timeout` (U4), DNS (U5) and library links in the manifest (U8) follow in their own ADRs.
- Scope: `lib/std`'s async sockets, the built-in blocking `TcpListener` and `TcpStream` (type-system.md §26.5), the
  built-in `IoError` (§26.2), the hosted runtime's socket calls on every reactor, and the interpreter's models of them.

## 1. Context

CeeSafe's sockets speak IPv4 only, with no options:

- An address is dotted-quad text and a port; the runtime parses it into a `sockaddr_in` (`ceesafe_rt_io.c`,
  `ceesafe_ipv4`; `ceesafe_rt_async.c`'s listen, connect and accept). AcceptEx's address buffer has room for IPv4.
- The listen backlog is 16 (`ceesafe_rt_io.c`, both `listen` calls). A server under a burst of connections refuses
  them while the C library's own limit is far larger.
- `SO_REUSEADDR` (listeners) and `SO_NOSIGPIPE` (macOS) are set inside the runtime. Nothing else can be: no
  `TCP_NODELAY`, keep-alive, buffer sizes, linger, `SO_REUSEPORT` or `IPV6_V6ONLY`.
- Neither end's address can be read except the listener's port (`port()`), and only the write side can be shut down.

Errors: `IoError` is a built-in enum of eleven variants (§26.2). A connection reset, an abort, a timeout or an
unreachable host is `Other(errno)`, whose number differs between Linux, macOS and Windows (the runtime already
translates Winsock's codes to errno values, `cee_wsa_errno`, but folds `WSAEHOSTUNREACH` into `ENETUNREACH`). A server
must tell a peer that reset the connection from a bug of its own, portably.

A library cannot add any of this: the operations the async types are built from are the runtime's, and the async
primitives are library-only (§40.4, N0345). So it is CeeSafe's work, designed here.

Two facts constrain the design:

- **`TcpListener`, `TcpStream` and `IoError` are compiler built-ins** (`src/check/io.cee`, `src/check/declare.cee`,
  `src/check/structs.cee`), with interpreter models (`src/interpreter/io.cee`). The async types are lib/std.
- **The bootstrap.** The tree is built by stage 0, ceesafec 0.3.2 (Decision 0009), whose `IoError` has eleven
  variants. The compiler's own sources match on `IoError` (the interpreter's `errnoOf`). Both 0.3.2 and 0.6.0 accept a
  wildcard arm after a match that is already exhaustive (checked 2026-11-02), so those matches gain a `_` arm first, and
  the tree keeps building with stage 0 after the variants exist.

## 2. Options

**Where the address type lives.**

| | A. lib/std enum (proposed) | B. Built-in enum | C. Text only |
|---|---|---|---|
| What | `SocketAddress` in `lib/std/net.cee`, used by the async types | a built-in like `IoError`, usable by the built-in blocking types too | every API keeps taking and returning text |
| Cost | the library and the runtime's C record | the checker, both backends, the interpreter, for a type a library can define | parsing on every call; no typed peer address |

**A is proposed.** The address is plain data, which is what lib/std is for (Decision 0008). The built-in blocking types
take IPv6 text where they take IPv4 text today and get the options, but not the typed addresses (§3.6, question 1).

**Where the new error kinds go.** A new variant appended after `Other` keeps every existing tag value (the runtime's
`CEE_IO_*` numbers, a C program reading an exported `IoError`); a variant inserted before `Other` reads better in the
declaration but moves `Other`'s tag. **Appending is proposed**; the specification lists them in a sensible order
anyway.

## 3. Proposed decision

### 3.1 `SocketAddress` (lib/std)

```
public enum SocketAddress {
    V4([u8; 4], u16),            // address bytes in network order, port
    V6([u8; 16], u16, u32),      // address bytes, port, scope id (0: none)
}
```

- `SocketAddress.parse(text: StringView) -> SocketAddress?`: `a.b.c.d:port`, `[v6]:port`, `[v6%zone]:port`, where
  `v6` is any RFC 4291 text form (`::`, embedded IPv4 `::ffff:1.2.3.4`) and `zone` a number or an interface name.
  A name is resolved by the runtime (`if_nametoindex`; `ConvertInterfaceNameToLuidA` + `ConvertInterfaceLuidToIndex`
  on Windows), the only part of parsing that is not pure. Null if the text is not an address.
- `SocketAddress.parseIp(text, port) -> SocketAddress?`: an address without a port (`127.0.0.1`, `::1`, `fe80::1%2`).
- `a.write(w: borrow mut TextWriter)` (lib/core's writer): `127.0.0.1:8080`, `[::1]:8080`, `[fe80::1%2]:8080`, IPv6
  in RFC 5952's canonical form (lowercase, the longest run of zeros as `::`, embedded IPv4 for `::ffff:0:0/96`). The
  scope is written as its number.
- `a.port()`, `a.withPort(p)`, `a.isLoopback()`, `a.isUnspecified()`; `SocketAddress.v4Any(port)`,
  `v6Any(port)`, `v4Loopback(port)`, `v6Loopback(port)`.

It crosses into the runtime as a C record, `struct ceesafe_socket_address { uint32_t family; uint16_t port; uint32_t
scope; uint8_t bytes[16]; }` (family 4 or 6), which the runtime converts to and from `sockaddr_storage`. CeeSafe
code never sees `sockaddr`.

### 3.2 Listeners

```
public struct ListenOptions {
    public backlog: u32;         // 0: the platform's maximum (SOMAXCONN)
    public reuseAddress: bool;   // SO_REUSEADDR (default true, as today on POSIX)
    public reusePort: bool;      // SO_REUSEPORT (default false)
    public v6Only: bool;         // IPV6_V6ONLY for a V6 address (default false: dual stack)
}
```

- `ListenOptions.defaults()`; `AsyncTcpListener.bindWith(address: SocketAddress, options: ListenOptions) ->
  Result<AsyncTcpListener, IoError>`. `bind(address: StringView, port: u16)` stays, now taking IPv6 text too (`::`,
  `::1`) and the defaults, so its backlog becomes SOMAXCONN.
- `listener.localAddress() -> Result<SocketAddress, IoError>`; `port()` stays.
- On Windows `reuseAddress` is ignored, as the runtime does today: Winsock's `SO_REUSEADDR` lets another socket
  take over a bound port, which is not what the POSIX option means (a listener there may rebind past `TIME_WAIT`
  without it). `reusePort` where the platform has no load-balancing `SO_REUSEPORT` (Windows) is refused with
  `Unsupported` (§3.4), not silently ignored: a server that asks for it expects the kernel to spread connections.
- `v6Only` is always set explicitly on a V6 socket, so a program behaves the same on Linux (default off) and Windows
  (default on). With dual stack, an IPv4 peer's address is reported as `V4`, not as `::ffff:a.b.c.d` (question 2).

### 3.3 Streams

`AsyncTcpStream.connectTo(address: SocketAddress)`; `connect(address: StringView, port)` stays, taking IPv6 text too.
Every one of these is synchronous and returns `Result<…, IoError>`:

| Method | Option |
|---|---|
| `setNoDelay(on: bool)` | `TCP_NODELAY` |
| `setKeepAlive(idleSeconds: u32?)` | `SO_KEEPALIVE`, and with a value the idle time (`TCP_KEEPIDLE`; macOS `TCP_KEEPALIVE`; Windows `SIO_KEEPALIVE_VALS`); null turns it off |
| `setSendBuffer(bytes: u32)`, `setRecvBuffer(bytes: u32)` | `SO_SNDBUF`, `SO_RCVBUF` (the kernel may round; Linux doubles) |
| `setLinger(seconds: u32?)` | `SO_LINGER`; `0` makes the close send a reset; null restores the default |
| `peerAddress()`, `localAddress()` | `getpeername`, `getsockname`, as `SocketAddress` |
| `shutdownRead()` | `shutdown(SHUT_RD)`; `shutdownWrite()` stays |

`AsyncSockets`, for code with its own wait loop, gains the same calls on
descriptors.

### 3.4 `IoError` kinds

Appended after `Other`, in this order: `ConnectionReset` (`ECONNRESET`, `WSAECONNRESET`), `ConnectionAborted`
(`ECONNABORTED`), `NotConnected` (`ENOTCONN`), `TimedOut` (`ETIMEDOUT`), `HostUnreachable` (`EHOSTUNREACH`; Winsock's
no longer folded into `ENETUNREACH`), `NetworkUnreachable` (`ENETUNREACH`), `AddressNotAvailable` (`EADDRNOTAVAIL`),
and one not in the request, **`Unsupported`** (`EOPNOTSUPP`, `ENOTSUP`, `ENOPROTOOPT`, `EAFNOSUPPORT`,
`EPROTONOSUPPORT`): what a platform without an option or an address family reports, so §3.2's refusal and an IPv6
address on a host without IPv6 are portable (question 3).

Each has a `message()`, the C library's wording ("Connection reset by peer", "Operation timed out" is macOS's and
"Connection timed out" Linux's: the specification fixes one, glibc's). `CEE_IO_*` gains 11–18; `ceesafe_io_kind`,
lib/std's `hostError`, and the interpreter's `errnoOf`/`ioKindOf` map them.

`IoError.TimedOut` is not `AsyncError.TimedOut`: the first is the kernel's (a connect or keep-alive gave up), the
second a deadline of the program's (U4).

**Compatibility.** A `match` on `IoError` that names every variant and has no `_` arm stops compiling (N0256). The
corpus and the guide are updated; `src/` gains wildcard arms first (§1). CeeSafe is before 1.0 and has no written
versioning rule; this ADR takes the usual one, that a change breaking source is a minor release: **0.7.0**, whose
notes say so (question 5).

### 3.5 The runtime, on every reactor

- `sockaddr_storage` everywhere; one conversion pair (`cee_address_to_storage`, `cee_address_from_storage`); CosmoOS's
  own layout (`COSMO_AF_INET`, already 16 address bytes) keeps its branch.
- Readiness reactors (epoll, kqueue, `poll()`, `WSAPoll`): the same non-blocking calls with the family's size.
- IOCP: the AcceptEx buffer becomes `2 * (sizeof(struct sockaddr_in6) + 16)`, read with `GetAcceptExSockaddrs`;
  ConnectEx binds the socket first to `INADDR_ANY` or `in6addr_any` by family.
- io_uring: `IORING_OP_ACCEPT` keeps a null address; `peerAddress` is `getpeername` on demand.
- Options are plain `setsockopt` calls on the descriptor (or `WSAIoctl` for Windows keep-alive), outside any operation
  record, so `ceesafe_async_op`'s size is unchanged (the record's layout is decided once, in U3's ADR).
- The blocking `ceesafe_tcp_*` calls share the conversion and the options.

### 3.6 The blocking built-ins

`TcpListener.bind` and `TcpStream.connect` accept IPv6 text and use SOMAXCONN; `TcpStream` gains `setNoDelay`,
`setKeepAlive`, `setSendBuffer`, `setRecvBuffer`, `setLinger` and `shutdownRead`, built-in methods like its
others. The typed forms (`bindWith`, `connectTo`, `peerAddress`, `localAddress`) are the async types' only: a built-in
cannot name a lib/std type, and making `SocketAddress` a built-in (option B) costs the checker, both backends and the
interpreter for what a library type does. A blocking program that needs them uses `Async.run` with the async types,
which is what a server does anyway (question 1).

### 3.7 The interpreter

The interpreter runs no C (ADR 0035 §3.6): each new runtime call has a model in `src/interpreter/io.cee` that performs
it through the interpreter's own host (`AsyncSockets`, `TcpStream`), as the existing socket models do. Interface-name
zones are resolved by the host's runtime call; the models are what the interpreter's `run --interp` row in the
contract suite exercises.

## 4. Consequences

- CeeSafe.Web's transport (its M1) can listen on IPv6 and dual stack, tune its sockets and classify network errors
  portably.
- A program matching `IoError` exhaustively must add the new arms (0.7.0's notes).
- New diagnostics: none. A bad option value (a buffer size above `i32`'s range) is `InvalidInput` at run time.
- `ceesafe_async_op` is unchanged; U3 decides its layout.

## 5. Questions for the owner

1. The blocking `TcpListener`/`TcpStream` get IPv6 text, SOMAXCONN and the setters, but not `SocketAddress`
   (§3.6)? Recommended: yes. (The alternative, a built-in `SocketAddress`, is option B.)
2. Dual-stack listeners by default (`v6Only: false`), and IPv4-mapped peers reported as `V4`? Recommended: yes.
3. Add `Unsupported` to the seven requested kinds, and refuse `reusePort` on Windows with it? Recommended: yes.
4. Append the new variants after `Other`, keeping every existing tag? Recommended: yes.
5. Release this, with U3 and U4, as ceesafec **0.7.0** (the request said 0.6.0, which shipped on 2026-11-02 with ADR
   0043)? Recommended: yes.

## 6. Implementation plan (once accepted)

1. Spec: §26.2 (the kinds and messages), §26.5 (the blocking types' additions), §40.4 (the async table and
   `SocketAddress`), `c-backend.md` §6 (the runtime calls and the address record), the API pages.
2. `src/`: wildcard arms on every `IoError` match first; then the variants (`declare.cee`, `structs.cee`), the
   interpreter's mappings and models, and `TcpStream`'s new built-in methods (`check/io.cee`, both backends).
3. Runtime: the conversions, `ceesafe_io_kind`, the options, IPv6 on each reactor; `tools/gen_runtime.py`.
4. lib/std: `net.cee` (`SocketAddress`, `ListenOptions`), the async types' methods, `AsyncSockets`;
   `tools/gen_library.py`.
5. Tests, with expected values from oracles (Python's `ipaddress` for parsing and RFC 5952 text; a C harness reading
   each option back with `getsockopt`):
   - corpus run-pass: IPv6 loopback accept and connect; dual stack with an IPv4 peer reported as `V4`; peer address
     equal to the connector's local address; every option round-trip; a backlog of 1000 simultaneous connects, all
     accepted; a reset (`setLinger(0)` then drop) read as `ConnectionReset`, and written as `ConnectionReset` or
     `BrokenPipe` as the platform reports (the specification says which);
   - check-fail: an `IoError` match naming only the eleven old variants (N0256);
   - every reactor: the contract suite's runs, and its "reactor uring" step with `CEESAFE_REACTOR_URING=1`; Windows
     (IOCP and `WSAPoll`) on the Windows 11 ARM64 test host;
   - ASan/UBSan, the allocation-failure sweep with `--check-leaks`, and `tools/cancel_sweep.py` over the new tests.
6. The guide (`guide/async.md`, "Waiting for the outside world", and the sockets section), ROADMAP §10, a release.
