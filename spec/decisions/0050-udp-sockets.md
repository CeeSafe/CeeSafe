# ADR 0050 — UDP sockets for tasks

- Status: **Accepted** by the owner, 2026-11-02 ("Yes, I accept all five ADRs with your recommendations"): every
  question of §5 as recommended. Asked for by the owner for CeeSafe.Web
  (`~/dev/CeeSafe.Web/docs/ceesafe-upstream-work.md`, item U6). Its HTTP/3 (QUIC) and its own DNS client (phase B of
  U5) need UDP.
- Scope: lib/std's `AsyncUdpSocket`, the hosted runtime's datagram operations on every reactor, the interpreter's
  model. It uses ADR 0045's message record, which was sized for this.

## 1. Context

CeeSafe has TCP only. QUIC needs four things beyond a plain datagram send and receive:

- **The local address of a received datagram:** on a server bound to `[::]` with several addresses, a reply must leave
  from the address the client used (`IP_PKTINFO`, `IPV6_RECVPKTINFO`).
- **ECN bits:** both received (`IP_RECVTOS`, `IPV6_RECVTCLASS`) and sent.
- **Truncation:** a datagram larger than the buffer must be reported, not silently cut.
- **Sending many datagrams at once:** generic segmentation offload (`UDP_SEGMENT` on Linux, USO on Windows). This is
  an optimisation, with a plain loop elsewhere.

These are `recvmsg`/`sendmsg` with control messages on POSIX and io_uring, and `WSARecvMsg`/`WSASendMsg` on Windows.
ADR 0045 already gave the message record room for the message header, a peer address and a control buffer (128
bytes; 96 on Windows), so the record is settled.

One fact constrains the interpreter. Its socket models call the *host's* lib/std (ADR 0044 §3.7), and stage 0 (ceesafec
0.3.2) has no UDP. The models therefore need a bootstrap chain step: one commit adds the library functions, the next
uses them. That is the same pattern ADR 0044 used (`f7a7d57`, `93c433f`).

## 2. Options

**One call with every option, or simple calls plus a message call.**

| | A. `sendTo`/`recvFrom`, plus `sendMsg`/`recvMsg` (proposed) | B. Messages only |
|---|---|---|
| What | the common case is two plain calls; QUIC uses the message calls with their fields | every send and receive is a message with optional fields |
| Cost | four operations | every caller fills or ignores fields it does not use |

**A is proposed.** DNS needs only `sendTo` and `recvFrom`. QUIC needs the message calls.

## 3. Proposed decision

### 3.1 The API (lib/std, `net.cee` and `async.cee`)

```ceesafe
public struct UdpOptions {
    public reuseAddress: bool;     // SO_REUSEADDR (ignored on Windows, as for TCP)
    public reusePort: bool;        // SO_REUSEPORT; Unsupported on Windows
    public v6Only: bool;           // IPV6_V6ONLY, set explicitly on an IPv6 socket
    public sendBuffer: u32;        // SO_SNDBUF, 0: the system's
    public recvBuffer: u32;        // SO_RCVBUF, 0: the system's
    public receiveEcn: bool;       // IP_RECVTOS / IPV6_RECVTCLASS: recvMsg reports ECN
    public receiveLocal: bool;     // IP_PKTINFO / IPV6_RECVPKTINFO: recvMsg reports the local address
    public fn defaults() -> UdpOptions;   // everything off, buffers the system's
}

/// What recvFrom gives.
public struct Datagram { public length: usize; public from: SocketAddress; public truncated: bool; }

/// What recvMsg gives.
public struct UdpReceived {
    public length: usize;            // bytes in the buffer
    public truncated: bool;          // the datagram was longer (MSG_TRUNC); the rest is gone
    public from: SocketAddress;
    public to: SocketAddress?;       // the local address it arrived at (with receiveLocal)
    public ecn: u8;                  // the ECN bits, 0 to 3 (with receiveEcn), else 0
}

@noncopy
public struct AsyncUdpSocket {
    public fn bind(address: SocketAddress, options: UdpOptions) -> Result<AsyncUdpSocket, IoError>;
    public fn localAddress(borrow this) -> Result<SocketAddress, IoError>;
    public async fn sendTo(borrow this, bytes: Slice<u8>, to: SocketAddress) -> Result<void, AsyncError>;
    public async fn recvFrom(borrow this, buffer: SliceMut<u8>) -> Result<Datagram, AsyncError>;
    public async fn sendMsg(borrow this, message: borrow UdpMessage) -> Result<usize, AsyncError>;
    public async fn recvMsg(borrow this, buffer: SliceMut<u8>) -> Result<UdpReceived, AsyncError>;
}

/// A datagram to send with sendMsg: the parts (ADR 0045's IoParts), where to, and optionally from where, its ECN
/// bits, and a segment size.
public struct UdpMessage<region r> {
    public parts: IoParts<r>;
    public to: SocketAddress;
    public from: SocketAddress?;   // the source address (IP_PKTINFO); null: the system's choice
    public ecn: u8;                // 0: not set
    public segmentSize: u16;       // 0: one datagram; else the parts are sent as datagrams of this size (GSO)
}
```

- **Sending.** `sendTo` sends one datagram whole, or fails: a datagram is never partly sent. `EMSGSIZE` is
  `InvalidInput`.
- **Segments.** `sendMsg` with `segmentSize > 0` sends `ceil(total / segmentSize)` datagrams and returns how many
  bytes it sent:
  - Linux: one call with `UDP_SEGMENT`;
  - Windows: `UDP_SEND_MSG_SIZE` where the system has it;
  - elsewhere: one send per segment.

  Under backpressure it may stop between datagrams: the count says how many segments went, and never a part of one.
- **Receiving.** `recvFrom` and `recvMsg` wait for one datagram. A datagram longer than the buffer fills it and is
  reported `truncated`.
- **Addresses.** An IPv4-mapped address received on a dual-stack socket is reported as `V4`, as for TCP (ADR 0044).
- **Errors** are ADR 0044's `IoError` kinds. A port unreachable that the system reports on a later receive
  (`ECONNREFUSED`, `WSAECONNRESET` on Windows) is `ConnectionRefused` on that receive. Windows' reporting of it is
  turned off (`SIO_UDP_CONNRESET`), as other runtimes do, so a server's receive loop is not broken by one bad peer.

### 3.2 The runtime, on every reactor

- **Readiness (epoll, kqueue, `WSAPoll`):** non-blocking `recvmsg`/`sendmsg` (`WSARecvMsg`/`WSASendMsg`), registering
  for readability or writability on `EAGAIN`.
- **IOCP:** overlapped `WSARecvMsg` (found with `WSAIoctl`, `WSAID_WSARECVMSG`) and `WSASendMsg`. The message header,
  the peer address and the control buffer are in the message record (ADR 0045).
- **io_uring:** `IORING_OP_RECVMSG` and `IORING_OP_SENDMSG`, with the record's `msghdr`.
- **Control messages** are parsed and built in the runtime:
  - packet info (`IP_PKTINFO`, `IPV6_PKTINFO`; macOS's `IP_RECVDSTADDR` for IPv4 where `IP_PKTINFO` is absent);
  - traffic class (`IP_TOS`, `IPV6_TCLASS`; Windows `IP_ECN`, `IPV6_ECN`);
  - segment size.

  A sent ECN value is set per message where the system takes it as a control message (Linux, Windows). Elsewhere it
  is set on the socket before the send, which is correct while one task sends on the socket at a time, as the type's
  `borrow this` operations allow.
- **CosmoOS userland** has no UDP in its socket layer yet: `bind` is `Unsupported`.

### 3.3 The interpreter

The interpreter models the operations with the host lib/std's raw `AsyncSockets` UDP calls (ADR 0044 §3.7's pattern),
which this ADR adds. So the bootstrap gains one chain step: the library commit, then the interpreter commit that uses
it. Control messages are modelled for packet info and truncation; ECN and segments are reported as 0 and sent as
plain datagrams there.

## 4. Consequences

- CeeSafe.Web can write QUIC and a DNS client over UDP.
- The bootstrap chain grows by one step, until stage 0 moves to a release with UDP (Decision 0009).
- New diagnostics: none.

## 5. Questions for the owner

1. `sendTo`/`recvFrom` for the common case, plus `sendMsg`/`recvMsg` with the local address, ECN, truncation and
   segments (option A)? Recommended: yes.
2. A datagram is sent whole or not at all, and a segmented send reports whole segments only? Recommended: yes.
3. Windows' port-unreachable reports on receive turned off (`SIO_UDP_CONNRESET`)? Recommended: yes.
4. A batched receive (`recvmmsg`) and connected UDP sockets left for later, when QUIC measures the need? Recommended:
   yes.

## 6. Implementation plan (once accepted)

1. Spec: §40.4 (the UDP types), c-backend.md §6 (the operations and the control messages), the guide.
2. Runtime: the datagram socket, the options, the operations on each backend, control messages; `tools/gen_runtime.py`.
3. lib/std: the types and operations, and `AsyncSockets`' raw UDP calls; `tools/gen_library.py`. One commit, which
   becomes a chain step.
4. The interpreter's models (the next commit); `tools/bootstrap_release.sh`'s chain and `bootstrap/reference.md`.
5. Tests:
   - corpus run-pass: a round trip on IPv4 and on IPv6 loopback; a truncated datagram flagged; `from` and `to` on
     loopback v4 and v6 (the local address reported); ECN bits sent and seen; a segmented send received as the
     expected datagrams; an empty datagram;
   - a receive cancelled in flight under ASan, and `tools/cancel_sweep.py`;
   - expected datagram contents from a Python oracle (the bytes and their split into segments);
   - every reactor, the contract suite on macOS, Debian (io_uring) and Windows 11 ARM64 (IOCP, `WSAPoll`).
6. ROADMAP §10.
