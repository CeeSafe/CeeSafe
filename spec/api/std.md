# lib/std

- [struct AsyncConsole](#struct-asyncconsole)
- [struct AsyncTcpListener](#struct-asynctcplistener)
- [struct AsyncTcpStream](#struct-asynctcpstream)
- [struct AsyncUdpSocket](#struct-asyncudpsocket)
- [struct AsyncSockets](#struct-asyncsockets)
- [struct AsyncFiles](#struct-asyncfiles)
- [enum SendError](#enum-senderror)
- [struct Channel](#struct-channel)
- [struct AsyncMutex](#struct-asyncmutex)
- [struct AsyncMutexGuard](#struct-asyncmutexguard)
- [struct Crc32c](#struct-crc32c)
- [struct Crc64Nvme](#struct-crc64nvme)
- [struct Md5](#struct-md5)
- [struct Sha256](#struct-sha256)
- [struct HmacSha256](#struct-hmacsha256)
- [interface HashKey](#interface-hashkey)
- [struct Hasher](#struct-hasher)
- [fn deriveHash](#fn-derivehash)
- [struct HashMap](#struct-hashmap)
- [enum PathKind](#enum-pathkind)
- [struct ProcessOutput](#struct-processoutput)
- [struct Environment](#struct-environment)
- [struct OpenFileLimit](#struct-openfilelimit)
- [struct ProcessLimits](#struct-processlimits)
- [struct VolumeSpace](#struct-volumespace)
- [struct Directory](#struct-directory)
- [struct HostServices](#struct-hostservices)
- [struct FileMetadata](#struct-filemetadata)
- [struct UnixTime](#struct-unixtime)
- [struct Clock](#struct-clock)
- [struct Random](#struct-random)
- [enum SocketAddress](#enum-socketaddress)
- [interface SocketEndpoint](#interface-socketendpoint)
- [struct SocketAddresses](#struct-socketaddresses)
- [struct ListenOptions](#struct-listenoptions)
- [struct IoParts](#struct-ioparts)
- [struct UdpOptions](#struct-udpoptions)
- [struct Datagram](#struct-datagram)
- [struct UdpReceived](#struct-udpreceived)
- [struct UdpMessage](#struct-udpmessage)
- [struct AsyncDns](#struct-asyncdns)
- [interface OrderKey](#interface-orderkey)
- [fn deriveCompare](#fn-derivecompare)
- [struct OrderedCursor](#struct-orderedcursor)
- [struct OrderedMap](#struct-orderedmap)
- [struct Mutex](#struct-mutex)
- [struct MutexGuard](#struct-mutexguard)
- [struct Arc](#struct-arc)
- [enum SpawnError](#enum-spawnerror)
- [struct OnceLock](#struct-oncelock)
- [struct TestFailure](#struct-testfailure)
- [struct Check](#struct-check)
- [struct CheckResult](#struct-checkresult)
- [struct TestRun](#struct-testrun)
- [enum WindowError](#enum-windowerror)
- [struct WindowSize](#struct-windowsize)
- [struct WindowBox](#struct-windowbox)
- [enum WindowButton](#enum-windowbutton)
- [struct WindowModifiers](#struct-windowmodifiers)
- [enum WindowKey](#enum-windowkey)
- [enum WindowCursor](#enum-windowcursor)
- [enum WindowEvent](#enum-windowevent)
- [struct Window](#struct-window)

## struct AsyncConsole

```ceesafe
public struct AsyncConsole
```

Console output for tasks: in order with Console's, and never blocking the thread (ADR 0031 §3.5).

### AsyncConsole.write

```ceesafe
public async fn write(text: StringView) -> Result<void, AsyncError>
```

Writes `text` to standard output.

### AsyncConsole.writeLine

```ceesafe
public async fn writeLine(text: StringView) -> Result<void, AsyncError>
```

Writes `text` and a newline to standard output.

### AsyncConsole.writeError

```ceesafe
public async fn writeError(text: StringView) -> Result<void, AsyncError>
```

Writes `text` to standard error.

## struct AsyncTcpListener

```ceesafe
@noncopy
public struct AsyncTcpListener {
    drop { … }
}
```

A TCP listener for tasks (type-system.md §40.4): `accept` parks the task until a connection arrives. Its socket is
non-blocking; the drop closes it.

### AsyncTcpListener.bind

```ceesafe
public fn bind(address: StringView, port: u16) -> Result<AsyncTcpListener, IoError>
```

Listens on `address` (IPv4 or IPv6 text, without a zone) and `port` (0: any free port), with
`ListenOptions.defaults()`.
Safety: the runtime reads the address only during the call.

### AsyncTcpListener.bindWith

```ceesafe
public fn bindWith(address: SocketAddress, options: ListenOptions) -> Result<AsyncTcpListener, IoError>
```

Listens on `address` with `options` (ADR 0044 §3.2).
Safety: the runtime reads the address only during the call.

### AsyncTcpListener.localAddress

```ceesafe
public fn localAddress(borrow this) -> Result<SocketAddress, IoError>
```

The address it listens on.

### AsyncTcpListener.port

```ceesafe
public fn port(borrow this) -> Result<u16, IoError>
```

The port it listens on.
Safety: the descriptor is this listener's.

### AsyncTcpListener.accept

```ceesafe
public async fn accept(borrow this) -> Result<AsyncTcpStream, AsyncError>
```

The next connection; the task parks until one arrives.
Safety: the descriptor is this listener's; a new one becomes the stream's.

## struct AsyncTcpStream

```ceesafe
@noncopy
public struct AsyncTcpStream {
    drop { … }
}
```

A TCP stream for tasks (type-system.md §40.4): `read` and `write` park the task until the socket is ready. Its
socket is non-blocking; the drop closes it.

### AsyncTcpStream.connect

```ceesafe
public async fn connect(address: StringView, port: u16) -> Result<AsyncTcpStream, AsyncError>
```

Connects to `address` (IPv4 or IPv6 text, without a zone) and `port`; the task parks until the connection is
made or refused.
Safety: the runtime reads the address only during the call; the descriptor becomes the stream's at once, so an
error closes it.

### AsyncTcpStream.connectTo

```ceesafe
public async fn connectTo(address: SocketAddress) -> Result<AsyncTcpStream, AsyncError>
```

Connects to `address` (ADR 0044 §3.3); the task parks until the connection is made or refused.
Safety: the runtime reads the address only during each step; the descriptor becomes the stream's at once.

### AsyncTcpStream.setNoDelay

```ceesafe
public fn setNoDelay(borrow this, on: bool) -> Result<void, IoError>
```

TCP_NODELAY: send small writes at once instead of gathering them (Nagle's algorithm off).

### AsyncTcpStream.setKeepAlive

```ceesafe
public fn setKeepAlive(borrow this, idleSeconds: u32?) -> Result<void, IoError>
```

SO_KEEPALIVE: probe an idle connection, after `idleSeconds` of quiet (0: the system's time); null: off.

### AsyncTcpStream.setSendBuffer

```ceesafe
public fn setSendBuffer(borrow this, bytes: u32) -> Result<void, IoError>
```

SO_SNDBUF: the kernel's send buffer, in bytes (it may round the size, and Linux doubles it).

### AsyncTcpStream.setRecvBuffer

```ceesafe
public fn setRecvBuffer(borrow this, bytes: u32) -> Result<void, IoError>
```

SO_RCVBUF: the kernel's receive buffer, in bytes (as setSendBuffer).

### AsyncTcpStream.setLinger

```ceesafe
public fn setLinger(borrow this, seconds: u32?) -> Result<void, IoError>
```

SO_LINGER: how long the close waits for unsent data, in seconds; 0 makes the close reset the connection;
null restores the default (the close returns at once and the kernel sends the rest).

### AsyncTcpStream.peerAddress

```ceesafe
public fn peerAddress(borrow this) -> Result<SocketAddress, IoError>
```

The peer's address (an IPv4 peer of a dual-stack listener as V4).

### AsyncTcpStream.localAddress

```ceesafe
public fn localAddress(borrow this) -> Result<SocketAddress, IoError>
```

This end's address.

### AsyncTcpStream.shutdownRead

```ceesafe
public fn shutdownRead(borrow this) -> Result<void, IoError>
```

Ends this side's reading: what arrives afterwards is discarded, and a read returns 0.
Safety: the descriptor is this stream's.

### AsyncTcpStream.read

```ceesafe
public async fn read(borrow this, buffer: SliceMut<u8>) -> Result<usize, AsyncError>
```

Reads into `buffer`: the count, 0 at the end of the stream; the task parks until there is something to read.
Safety: the runtime writes at most the buffer's length, during the call only.

### AsyncTcpStream.write

```ceesafe
public async fn write(borrow this, bytes: Slice<u8>) -> Result<void, AsyncError>
```

Writes every byte; the task parks whenever the socket cannot take more.
Safety: the runtime reads at most the slice's length, during the call only.

### AsyncTcpStream.writeVectored

```ceesafe
public async fn writeVectored(borrow this, parts: borrow IoParts) -> Result<usize, AsyncError>
```

Sends from `parts` in one step (ADR 0045 §3.2): the task parks until the socket takes at least one byte, and the
result is how many it took, which may stop inside any part. Empty parts send nothing and return 0.

### AsyncTcpStream.writeAllVectored

```ceesafe
public async fn writeAllVectored(borrow this, parts: borrow IoParts) -> Result<void, AsyncError>
```

Sends every byte of `parts`, in as many steps as the socket needs.

### AsyncTcpStream.sendFile

```ceesafe
public async fn sendFile(borrow this, file: borrow File, offset: u64, length: u64) -> Result<u64, AsyncError>
```

Sends from `file` at `offset`, at most `length` bytes, in one step (ADR 0051): the task parks until the socket
takes at least one byte, and the result is how many went, 0 if the file ends at `offset`. The file's own position
is neither used nor moved, so one file may be sent on several streams at once. Where the system cannot send a
file (ceesafe_async_op_sendfile's CEE_ASYNC_NO_SENDFILE), the step reads at most 64 KiB at `offset` on a worker
thread and writes them: the same results, through a copy.
Safety: the runtime reads the file's descriptor during the call only, and lends the kernel no buffer.

### AsyncTcpStream.sendFileAll

```ceesafe
public async fn sendFileAll(borrow this, file: borrow File, offset: u64, length: u64) -> Result<void, AsyncError>
```

Sends `length` bytes of `file` from `offset`, in as many steps as the socket needs. If the file ends first, the
result is `Io(InvalidInput)`, with the bytes before its end sent.

### AsyncTcpStream.shutdownWrite

```ceesafe
public fn shutdownWrite(borrow this) -> Result<void, IoError>
```

Ends this side's writing, so the peer reads the end of the stream.
Safety: the descriptor is this stream's.

## struct AsyncUdpSocket

```ceesafe
public struct AsyncUdpSocket {
    drop { … }
}
```

A UDP socket for tasks (ADR 0050): datagrams sent whole or not at all, and received one at a time, a longer one than
the buffer reported as truncated. `sendTo` and `recvFrom` are the common case; `sendMsg` and `recvMsg` add the
local address, ECN bits and segments that QUIC needs.

### AsyncUdpSocket.bind

```ceesafe
public fn bind(address: SocketAddress, options: UdpOptions) -> Result<AsyncUdpSocket, IoError>
```

A socket bound to `address` (port 0: the system chooses), with `options`.
Safety: the runtime reads the address only during the call; the descriptor becomes the socket's at once, so an
error closes it.

### AsyncUdpSocket.localAddress

```ceesafe
public fn localAddress(borrow this) -> Result<SocketAddress, IoError>
```

The address it is bound to.

### AsyncUdpSocket.sendTo

```ceesafe
public async fn sendTo(borrow this, bytes: Slice<u8>, to: SocketAddress) -> Result<void, AsyncError>
```

Sends `bytes` as one datagram to `to`: whole, or not at all (a datagram too long for the path is
`Io(InvalidInput)`). An empty slice sends an empty datagram.

### AsyncUdpSocket.recvFrom

```ceesafe
public async fn recvFrom(borrow this, buffer: SliceMut<u8>) -> Result<Datagram, AsyncError>
```

Waits for one datagram, into `buffer`.

### AsyncUdpSocket.sendMsg

```ceesafe
public async fn sendMsg(borrow this, message: borrow UdpMessage) -> Result<usize, AsyncError>
```

Sends `message` (ADR 0050 §3.1): one datagram of its parts, or with `segmentSize` > 0 datagrams of that many
bytes each, the last one shorter, in one call where the system can (Linux's UDP_SEGMENT) and one by one
elsewhere. The result is the bytes sent: always whole segments. If a segment fails after others went, the result
is the bytes of those before it; if the first fails, its error.

### AsyncUdpSocket.recvMsg

```ceesafe
public async fn recvMsg(borrow this, buffer: SliceMut<u8>) -> Result<UdpReceived, AsyncError>
```

Waits for one datagram, into `buffer`, with what `UdpOptions` asked to be reported about it.
Safety: the runtime writes at most the buffer's length; the buffer stays borrowed until the operation is
complete, which this frame outlives (an operation abandoned by a cancellation is waited for, §40.7).

## struct AsyncSockets

```ceesafe
public struct AsyncSockets
```

The non-blocking socket calls under AsyncTcpListener and AsyncTcpStream, one at a time, on descriptors: a value
>= 0, `wouldBlock()`, or a negated errno. For code that runs its own wait loop, as the interpreter does; programs use
the async types, which own their descriptors.

### AsyncSockets.wouldBlock

```ceesafe
public fn wouldBlock() -> i64
```

### AsyncSockets.listen

```ceesafe
public fn listen(address: StringView, port: u16) -> i64
```

Safety: the runtime reads the address during the call only.

### AsyncSockets.port

```ceesafe
public fn port(fd: i32) -> i64
```

Safety: on a descriptor the runtime only asks the kernel.

### AsyncSockets.accept

```ceesafe
public fn accept(fd: i32) -> i64
```

Safety: as port.

### AsyncSockets.connect

```ceesafe
public fn connect(address: StringView, port: u16) -> i64
```

Safety: as listen.

### AsyncSockets.connected

```ceesafe
public fn connected(fd: i32) -> i64
```

Safety: as port.

### AsyncSockets.read

```ceesafe
public fn read(fd: i32, buffer: SliceMut<u8>) -> i64
```

Safety: the runtime writes at most the buffer's length, during the call only.

### AsyncSockets.send

```ceesafe
public fn send(fd: i32, bytes: Slice<u8>) -> i64
```

Safety: the runtime reads at most the slice's length, during the call only.

### AsyncSockets.shutdownWrite

```ceesafe
public fn shutdownWrite(fd: i32) -> i64
```

Safety: as port.

### AsyncSockets.listenAt

```ceesafe
public fn listenAt(family: u32, bytes: Slice<u8>, port: u16, scope: u32, backlog: u32, flags: u32) -> i64
```

ADR 0044's calls on descriptors, for the interpreter's models: a listener at an address's fields (`bytes`
has 16, of which IPv4 uses 4), with a backlog and rt.h's CEE_LISTEN_* flags.
Safety: the runtime reads the 16 bytes during the call only (checked: fewer is -EINVAL).

### AsyncSockets.connectAt

```ceesafe
public fn connectAt(family: u32, bytes: Slice<u8>, port: u16, scope: u32) -> i64
```

A connection started to an address's fields, as `connect`.
Safety: as listenAt.

### AsyncSockets.name

```ceesafe
public fn name(fd: i32, peer: u32, bytes: SliceMut<u8>, scope: borrow mut u32) -> i64
```

The socket's own (`peer` 0) or its peer's (1) address: its 16 bytes and scope written; (family << 16) | port,
or a negated errno.
Safety: the runtime writes 16 bytes (checked) and the scope during the call only.

### AsyncSockets.setOption

```ceesafe
public fn setOption(fd: i32, option: u32, value: i64) -> i64
```

Sets rt.h's CEE_SOCKET_OPTION_* `option` to `value`: 0 or a negated errno.
Safety: as port.

### AsyncSockets.shutdownRead

```ceesafe
public fn shutdownRead(fd: i32) -> i64
```

Safety: as port.

### AsyncSockets.sendVectored

```ceesafe
public async fn sendVectored(fd: i32, parts: borrow IoParts) -> Result<usize, AsyncError>
```

One vectored send on a descriptor (ADR 0045 §3.2): the task parks until the socket takes at least one byte;
the count may stop inside any part. Empty parts send nothing and return 0.
Safety: the parts stay borrowed until the operation completes, and its record is in this task's frame.

### AsyncSockets.udpBind

```ceesafe
public fn udpBind(
    family: u32,
    bytes: Slice<u8>,
    port: u16,
    scope: u32,
    flags: u32,
    sendBuffer: u32,
    receiveBuffer: u32,
) -> i64
```

A UDP socket bound to the address's fields (ADR 0050), with UdpOptions' flags and the buffer sizes: the
descriptor, or a negated errno.
Safety: the runtime reads the 16 address bytes during the call only.

### AsyncSockets.udpSend

```ceesafe
public fn udpSend(
    fd: i32,
    data: Slice<u8>,
    socketFamily: u32,
    family: u32,
    bytes: Slice<u8>,
    port: u16,
    scope: u32,
) -> i64
```

One datagram of `data` to the address's fields, from a socket of `socketFamily`: the count, `wouldBlock()`, or
a negated errno.
Safety: the runtime reads the data and the 16 address bytes during the call only.

### AsyncSockets.udpRecv

```ceesafe
public fn udpRecv(
    fd: i32,
    buffer: SliceMut<u8>,
    bytes: SliceMut<u8>,
    scope: borrow mut u32,
    info: borrow mut u32,
) -> i64
```

One datagram into `buffer`: its length there, `wouldBlock()`, or a negated errno; the peer's 16 bytes and scope,
and in `info` (family << 24) | (truncated << 16) | port.
Safety: the runtime writes at most the buffer's length and 16 address bytes, during the call only.

### AsyncSockets.interfaceIndex

```ceesafe
public fn interfaceIndex(name: Slice<u8>) -> u32
```

An interface's index by name (an IPv6 zone), 0 if there is none (lib/std's net.cee declares the call).
Safety: the runtime reads the name during the call only.

### AsyncSockets.close

```ceesafe
public fn close(fd: i32)
```

Safety: as port.

### AsyncSockets.poll

```ceesafe
public fn poll(fds: Slice<i32>, events: Slice<u32>, ready: SliceMut<u32>, timeout: i64) -> i64
```

Waits up to `timeout` ms (-1: no limit) for any descriptor: `events[k]` 1 readable, 2 writable; `ready[k]`
receives each one's readiness. The count ready, 0 on timeout, or a negated errno.
Safety: the three slices have `fds.length` elements (checked), read and written during the call only.

## struct AsyncFiles

```ceesafe
public struct AsyncFiles
```

Files for tasks (type-system.md §40.4): a regular file is always ready to the kernel's readiness calls, so each
operation runs on a worker thread (Async.blocking) while the task waits.

### AsyncFiles.open

```ceesafe
public async fn open(path: StringView) -> Result<File, AsyncError>
```

Opens `path` for reading.

### AsyncFiles.openRegular

```ceesafe
public async fn openRegular(path: StringView) -> Result<File, AsyncError>
```

Opens `path` for reading if it is a regular file (ADR 0078 §3): never waits on a FIFO; `IsDirectory` for a
directory, `InvalidInput` for anything else that is not a regular file.

### AsyncFiles.modified

```ceesafe
public async fn modified(file: borrow mut File) -> Result<UnixTime, AsyncError>
```

The open file's last modification time, read from the file (ADR 0078 §3).

### AsyncFiles.kind

```ceesafe
public async fn kind(file: borrow mut File) -> Result<PathKind, AsyncError>
```

What the open file is, read from the file (ADR 0078 §3).

### AsyncFiles.create

```ceesafe
public async fn create(path: StringView) -> Result<File, AsyncError>
```

Creates or truncates `path` for writing.

### AsyncFiles.read

```ceesafe
public async fn read(file: borrow mut File, buffer: SliceMut<u8>) -> Result<usize, AsyncError>
```

Reads into `buffer`: the count, 0 at the end of the file.

### AsyncFiles.write

```ceesafe
public async fn write(file: borrow mut File, bytes: Slice<u8>) -> Result<void, AsyncError>
```

Writes every byte.

### AsyncFiles.openReadWrite

```ceesafe
public async fn openReadWrite(path: StringView) -> Result<File, AsyncError>
```

Opens an existing `path` for reading and writing (ADR 0075 §2.1).

### AsyncFiles.openOrCreate

```ceesafe
public async fn openOrCreate(path: StringView) -> Result<File, AsyncError>
```

Opens `path` for reading and writing, creating it if missing; never truncates (ADR 0075 §2.1).

### AsyncFiles.createNew

```ceesafe
public async fn createNew(path: StringView) -> Result<File, AsyncError>
```

Creates `path` for reading and writing; `AlreadyExists` if it exists (ADR 0075 §2.1).

### AsyncFiles.readAt

```ceesafe
public async fn readAt(file: borrow mut File, buffer: SliceMut<u8>, offset: u64) -> Result<usize, AsyncError>
```

One read at `offset` into `buffer`: the count, 0 at or past the end (ADR 0075 §2.1).

### AsyncFiles.length

```ceesafe
public async fn length(file: borrow mut File) -> Result<u64, AsyncError>
```

The file's length in bytes (ADR 0075 §2.1).

### AsyncFiles.writeAt

```ceesafe
public async fn writeAt(file: borrow mut File, bytes: Slice<u8>, offset: u64) -> Result<void, AsyncError>
```

Writes every byte at `offset` (ADR 0075 §2.1).

### AsyncFiles.syncData

```ceesafe
public async fn syncData(file: borrow mut File) -> Result<void, AsyncError>
```

Makes the data, and the metadata needed to read it, durable (ADR 0075 §2.1). After a failure the durable contents are unknown.

### AsyncFiles.syncAll

```ceesafe
public async fn syncAll(file: borrow mut File) -> Result<void, AsyncError>
```

`syncData` with all metadata (ADR 0075 §2.1).

### AsyncFiles.setLength

```ceesafe
public async fn setLength(file: borrow mut File, length: u64) -> Result<void, AsyncError>
```

Truncates or zero-extends the file to `length` bytes (ADR 0075 §2.1).

### AsyncFiles.rename

```ceesafe
public async fn rename(from: StringView, to: StringView) -> Result<void, AsyncError>
```

Renames `from` to `to`, atomically replacing it (ADR 0075 §2.1).

### AsyncFiles.syncDirectory

```ceesafe
public async fn syncDirectory(path: StringView) -> Result<void, AsyncError>
```

Makes a directory's entries durable (ADR 0075 §2.1).

## enum SendError

```ceesafe
public enum SendError<T> {
    Closed(T),
    Cancelled(T),
}
```

Why `sendAsync` gave a value back: the channel is closed, or the task was cancelled (ADR 0032 §3.3).

## struct Channel

```ceesafe
@threadSafe
@noncopy
public struct Channel<T: Send> {
    drop { … }
}
```

A bounded channel (ADR 0032): every handle may send and receive; `clone` makes another handle to the same channel,
and the last handle's drop drops the values still in it.

### Channel.create

```ceesafe
public fn create(capacity: usize) -> Result<Channel<T>, AllocError>
```

A channel of `capacity` values, its buffer allocated once. A capacity of 0 panics: rendezvous channels are not
in v1 (ADR 0032 §5).
Safety: the runtime's channel is reference counted; this handle holds one count until its drop.

### Channel.clone

```ceesafe
public fn clone(borrow this) -> Channel<T>
```

Another handle to the same channel.
Safety: it takes a count of the runtime's channel, given back by its drop.

### Channel.close

```ceesafe
public fn close(borrow this)
```

Every later send fails; receivers take what is buffered, then see the end.
Safety: the runtime's channel is alive while this handle is.

### Channel.isClosed

```ceesafe
public fn isClosed(borrow this) -> bool
```

Whether `close` has been called (ADR 0060). Values sent before it may still be buffered: the channel is drained
when `isClosed()` is true and a `tryRecv()` after it is `null`, in that order.
Safety: as `close`.

### Channel.send

```ceesafe
public fn send(borrow this, value: T) -> Result<void, T>
```

Sends `value`, waiting while the channel is full; `Err(value)` if it is closed.
Safety: a slot from begin_send is the channel's, under its lock until end_send, and holds no value.

### Channel.trySend

```ceesafe
public fn trySend(borrow this, value: T) -> Result<void, T>
```

Sends `value` if there is room now, without waiting: `Err(value)` if the channel is full or closed. It has no
effect, so a `drop` block may call it.
Safety: as `send`.

### Channel.tryRecv

```ceesafe
public fn tryRecv(borrow this) -> T?
```

The next value if one is buffered, else `null` at once, whether the channel is empty or closed and empty
(ADR 0060; `isClosed` tells the two apart). It has no effect, so async code and a `drop` block may call it.
Safety: as `recv`.

### Channel.recv

```ceesafe
public fn recv(borrow this) -> T?
```

The next value, waiting while the channel is empty; `null` once it is closed and empty.
Safety: a slot from begin_recv is the channel's, under its lock until end_recv, and holds a value.

### Channel.sendAsync

```ceesafe
public async fn sendAsync(borrow this, value: T) -> Result<void, SendError<T>>
```

Sends `value` from a task, parking it while the channel is full: other tasks of its thread run meanwhile. A
closed channel or a cancelled task gives the value back.
Safety: as `send`; the wait node is in this task's frame and leaves the queue before the task returns.

### Channel.recvAsync

```ceesafe
public async fn recvAsync(borrow this) -> Result<T?, AsyncError>
```

The next value for a task, parking it while the channel is empty; `null` once it is closed and empty;
`Err(AsyncError.Cancelled)` only while it has taken nothing, so no value is lost.
Safety: as `recv`; the wait node is in this task's frame and leaves the queue before the task returns.

## struct AsyncMutex

```ceesafe
@threadSafe
@noncopy
public struct AsyncMutex<T: Send>
```

Mutual exclusion for tasks (type-system.md §40.4): `await lock()` parks the task while another holds it, and the
guard may be held across `await`. Its token is the one value of a channel of capacity 1: taking it locks, and the
guard's drop puts it back without waiting.

### AsyncMutex.create

```ceesafe
public fn create(data: T) -> Result<AsyncMutex<T>, AllocError>
```

A mutex holding `data`, unlocked.

### AsyncMutex.lock

```ceesafe
public async fn lock(borrow this) -> Result<AsyncMutexGuard<T>, AsyncError>
```

Waits for the lock; `Err(AsyncError.Cancelled)` if the task is cancelled first.

## struct AsyncMutexGuard

```ceesafe
public struct AsyncMutexGuard<region r, T: Send> {
    drop { … }
}
```

Holding an AsyncMutex: `data()` lends the protected value; dropping the guard unlocks.

### AsyncMutexGuard.data

```ceesafe
public fn data(borrow mut this) -> borrow mut T
```

Safety: the guard holds the token, so no other task or thread reaches the data, and the loan ends before the
guard's drop.

## struct Crc32c

```ceesafe
public struct Crc32c
```

CRC-32C (Castagnoli: polynomial 0x1EDC6F41, reflected, initial value and final XOR all ones), the checksum of
iSCSI, ext4, Btrfs and the S3 `x-amz-checksum-crc32c` header. `Crc32c.of("123456789".bytes())` is 0xE3069283.

### Crc32c.create

```ceesafe
public fn create() -> Crc32c
```

A checksum of no bytes yet.

### Crc32c.update

```ceesafe
public fn update(borrow mut this, bytes: Slice<u8>)
```

Feeds `bytes`: through the CPU's CRC-32C instruction where it has one (SSE4.2 on x86-64, the CRC extension on
AArch64; ADR 0089), else eight at a time through the slicing tables, then the rest one at a time.
Safety: the runtime reads at most the slice's length, during the call only, and keeps no pointer.

### Crc32c.finish

```ceesafe
public fn finish(borrow this) -> u32
```

The checksum of the bytes fed so far. It does not change the state: more bytes may be fed after it.

### Crc32c.of

```ceesafe
public fn of(bytes: Slice<u8>) -> u32
```

The checksum of `bytes`, in one call.

## struct Crc64Nvme

```ceesafe
public struct Crc64Nvme
```

CRC-64/NVME (polynomial 0xAD93D23594C93659, reflected, initial value and final XOR all ones), the checksum of the
S3 `x-amz-checksum-crc64nvme` header. `Crc64Nvme.of("123456789".bytes())` is 0xAE8B14860A799888.

### Crc64Nvme.create

```ceesafe
public fn create() -> Crc64Nvme
```

A checksum of no bytes yet.

### Crc64Nvme.update

```ceesafe
public fn update(borrow mut this, bytes: Slice<u8>)
```

Feeds `bytes`: eight at a time through the slicing tables, then the rest one at a time.

### Crc64Nvme.finish

```ceesafe
public fn finish(borrow this) -> u64
```

The checksum of the bytes fed so far. It does not change the state: more bytes may be fed after it.

### Crc64Nvme.of

```ceesafe
public fn of(bytes: Slice<u8>) -> u64
```

The checksum of `bytes`, in one call.

## struct Md5

```ceesafe
public struct Md5
```

MD5 (RFC 1321): a 16-byte digest. **MD5 is broken as a signature**: collisions are cheap to make, so it must
never authenticate anything or stand for a value an attacker chooses. It is here for S3 ETags and `Content-MD5`
only, which detect accidental corruption. For anything else use Sha256 or HmacSha256.

### Md5.create

```ceesafe
public fn create() -> Md5
```

A digest of no bytes yet.

### Md5.update

```ceesafe
public fn update(borrow mut this, bytes: Slice<u8>)
```

Feeds `bytes`. Whole blocks are compressed straight from `bytes`; only a partial block is copied.

### Md5.finish

```ceesafe
public fn finish(borrow this) -> [u8; 16]
```

The digest of the bytes fed so far. It does not change the state: more bytes may be fed after it.

### Md5.of

```ceesafe
public fn of(bytes: Slice<u8>) -> [u8; 16]
```

The digest of `bytes`, in one call.

## struct Sha256

```ceesafe
public struct Sha256
```

SHA-256 (FIPS 180-4): a 32-byte digest.

### Sha256.create

```ceesafe
public fn create() -> Sha256
```

A digest of no bytes yet.

### Sha256.update

```ceesafe
public fn update(borrow mut this, bytes: Slice<u8>)
```

Feeds `bytes`. Whole blocks are compressed straight from `bytes`; only a partial block is copied.

### Sha256.finish

```ceesafe
public fn finish(borrow this) -> [u8; 32]
```

The digest of the bytes fed so far. It does not change the state: more bytes may be fed after it.

### Sha256.of

```ceesafe
public fn of(bytes: Slice<u8>) -> [u8; 32]
```

The digest of `bytes`, in one call.

## struct HmacSha256

```ceesafe
public struct HmacSha256
```

HMAC-SHA-256 (RFC 2104, FIPS 198-1): a 32-byte message authentication code under a key of any length. A key longer
than SHA-256's 64-byte block is first replaced by its SHA-256. Compare two codes in constant time, not with an
early exit, where an attacker can time the comparison.

### HmacSha256.create

```ceesafe
public fn create(key: Slice<u8>) -> HmacSha256
```

A code under `key` of no bytes yet.

### HmacSha256.update

```ceesafe
public fn update(borrow mut this, bytes: Slice<u8>)
```

Feeds `bytes` of the message.

### HmacSha256.finish

```ceesafe
public fn finish(borrow this) -> [u8; 32]
```

The code of the bytes fed so far. It does not change the state: more bytes may be fed after it.

### HmacSha256.of

```ceesafe
public fn of(key: Slice<u8>, bytes: Slice<u8>) -> [u8; 32]
```

The code of `bytes` under `key`, in one call.

## interface HashKey

```ceesafe
public interface HashKey<K> {
    fn hash(borrow this, h: borrow mut Hasher);
    fn sameAs(borrow this, other: borrow K) -> bool;
}
```

A key of a HashMap (ADR 0052 §3.1): `K: HashKey<K>`, since an interface has no `Self`.

## struct Hasher

```ceesafe
public struct Hasher
```

SipHash-1-3 (one compression round, three finalisation rounds), keyed by 128 bits: a hash an attacker cannot aim
collisions at without the key.

### Hasher.create

```ceesafe
public fn create(k0: u64, k1: u64) -> Hasher
```

A hasher keyed by `k0` and `k1`.

### Hasher.bytes

```ceesafe
public fn bytes(borrow mut this, data: Slice<u8>)
```

Feeds `data`. While nothing is pending, whole eight-byte words are compressed as they are read (the same
message blocks, little-endian, as byte by byte).

### Hasher.u64

```ceesafe
public fn u64(borrow mut this, value: u64)
```

Feeds `value`'s eight bytes, little-endian. With nothing pending they are one message block.

### Hasher.finish

```ceesafe
public fn finish(borrow this) -> u64
```

The hash of what was fed.

## fn deriveHash

```ceesafe
public fn deriveHash<K: HashKey<K>>(key: borrow K, h: borrow mut Hasher)
```

Feeds `key` to `h` through its `HashKey`: what a derived `hash` does for each field (ADR 0087 §2.3). A field type
without one is N0268 at the `@derive`.

## struct HashMap

```ceesafe
public struct HashMap<K: HashKey<K>, V>
```

A map from keys to values (ADR 0052 §3.2). Iteration (`keyAt`, `valueAt` for 0..length) is in insertion order, except
that a removal moves the last entry into the removed one's place. Only `put` allocates; a failed `put` leaves the map
as it was.

### HashMap.create

```ceesafe
public fn create() -> HashMap<K, V>
```

An empty map keyed by the process's random key.
Safety: the runtime writes two words into the array, which outlives the call.

### HashMap.withSeed

```ceesafe
public fn withSeed(k0: u64, k1: u64) -> HashMap<K, V>
```

An empty map keyed by `k0` and `k1`: for tests, and for keys no one chooses against it.

### HashMap.length

```ceesafe
public fn length(borrow this) -> usize
```

How many entries.

### HashMap.get

```ceesafe
public fn get(borrow this, key: borrow K) -> borrow(this) V?
```

The value of `key`, if it is there.

### HashMap.getMut

```ceesafe
public fn getMut(borrow mut this, key: borrow K) -> borrow mut(this) V?
```

The same, to change it.

### HashMap.contains

```ceesafe
public fn contains(borrow this, key: borrow K) -> bool
```

Whether `key` is there.

### HashMap.keyAt

```ceesafe
public fn keyAt(borrow this, i: usize) -> borrow(this) K
```

The key of entry `i` (0 ≤ i < length).

### HashMap.valueAt

```ceesafe
public fn valueAt(borrow this, i: usize) -> borrow(this) V
```

The value of entry `i` (0 ≤ i < length).

### HashMap.put

```ceesafe
public fn put(borrow mut this, key: K, value: V) -> Result<V?, AllocError>
```

Puts `value` at `key`: the old value if the key was there, else null. A failure leaves the map as it was. A map
holds at most 2^32 − 1 entries (a slot's entry field); one more is OutOfMemory.

### HashMap.remove

```ceesafe
public fn remove(borrow mut this, key: borrow K) -> V?
```

Takes `key` out: its value, or null. The last entry moves into its place (ADR 0052 §2).

### HashMap.clear

```ceesafe
public fn clear(borrow mut this)
```

Takes every entry out; the table keeps its size.

## enum PathKind

```ceesafe
public enum PathKind {
    File,
    Directory,
    Other,
}
```

What is at a path (`File.kind`, type-system.md §26.8).

## struct ProcessOutput

```ceesafe
public struct ProcessOutput {
    public status: i32;
    public stdout: Vector<u8>;
    public stderr: Vector<u8>;
}
```

A finished program's exit status and what it wrote (`Process.capture`, type-system.md §26.7).

## struct Environment

```ceesafe
public struct Environment
```

The environment the program started with (type-system.md §26.9).

### Environment.variable

```ceesafe
public fn variable(name: StringView) -> StringView?
```

The value of the variable `name` as static text, or null if it is not set or not valid UTF-8.
Safety: the runtime returns getenv's text, which stays valid and unchanged: CeeSafe has no way to change the
environment (C code that calls setenv breaks this, as it does in C).

## struct OpenFileLimit

```ceesafe
public struct OpenFileLimit {
    public current: u64;
    public maximum: u64;
}
```

The process's open-file limit (`ProcessLimits.openFiles`, ADR 0104). u64's maximum stands for "unlimited".

## struct ProcessLimits

```ceesafe
public struct ProcessLimits
```

Limits the system puts on the process (type-system.md §26.12, ADR 0104). Hosted only; the interpreter answers
`Unsupported`.

### ProcessLimits.openFiles

```ceesafe
public fn openFiles() -> Result<OpenFileLimit, IoError>
```

The open-file limit: POSIX `getrlimit(RLIMIT_NOFILE)`. Windows' handles have no count limit: both are u64's
maximum there.
Safety: the runtime writes the two values during the call only.

### ProcessLimits.raiseOpenFiles

```ceesafe
public fn raiseOpenFiles(to: u64) -> Result<u64, IoError>
```

Raises the soft open-file limit to `to`, at most the hard limit (and, on Apple systems, the kernel's
`kern.maxfilesperproc`, or `OPEN_MAX` where an older release refuses more), and returns the soft limit
afterwards. It never lowers it. A server calls it once at start: `ProcessLimits.raiseOpenFiles(u64.max)`.
Safety: the runtime writes the value during the call only.

## struct VolumeSpace

```ceesafe
public struct VolumeSpace {
    public total: u64;
    public free: u64;
    public available: u64;
}
```

Directories (type-system.md §26.10).
A volume's bytes (`Directory.space`, ADR 0077).

## struct Directory

```ceesafe
public struct Directory
```

### Directory.create

```ceesafe
public fn create(path: StringView) -> Result<void, IoError>
```

Makes one directory; `AlreadyExists` if something is there already.
Safety: the runtime reads the path's bytes during the call only.

### Directory.remove

```ceesafe
public fn remove(path: StringView) -> Result<void, IoError>
```

Removes an empty directory.
Safety: the runtime reads the path's bytes during the call only.

### Directory.space

```ceesafe
public fn space(path: StringView) -> Result<VolumeSpace, IoError>
```

The bytes of the volume that holds `path` (ADR 0077): `total`, `free`, and `available` to this process, which
is at most `free` where the system keeps a reserve for its administrator. POSIX `statvfs`; Windows
`GetDiskFreeSpaceExA`. A storage engine stops writing before `available` runs out. The interpreter answers
`Unsupported`.
Safety: the runtime reads the path's bytes and writes the three values during the call only.

### Directory.sync

```ceesafe
public fn sync(path: StringView) -> Result<void, IoError>
```

Makes the directory's entries durable: the files created, renamed or removed in it survive a power cut once
this returns (ADR 0075 §2.1). POSIX: `fsync` of the directory, `F_FULLFSYNC` on Apple systems. Windows journals
directory entries and cannot flush a directory: there it checks the path is a directory and does nothing else.
Safety: the runtime reads the path's bytes during the call only.

### Directory.list

```ceesafe
public fn list(path: StringView) -> Result<Vector<String>, IoError>
```

The names in a directory, without `.` and `..`, sorted by byte value so programs stay deterministic. A name
that is not valid UTF-8 is `InvalidInput`: skipping it could make a program miss a file without a word.
Safety: the listing is closed on every path out (`defer`), and each name is copied before the next call
reuses the runtime's buffer.

### Directory.temporary

```ceesafe
public fn temporary() -> Result<String, IoError>
```

A new, empty directory that only this user can enter, under the platform's temporary directory. The caller
removes it.
Safety: the runtime writes at most the buffer's length.

### Directory.current

```ceesafe
public fn current() -> Result<String, IoError>
```

The working directory.
Safety: the runtime writes at most the buffer's length.

## struct HostServices

```ceesafe
public struct HostServices
```

The functions behind `Process.capture` and `File.kind`.

### HostServices.capture

```ceesafe
public fn capture(
    program: StringView,
    arguments: borrow Vector<StringView>,
    input: Slice<u8>?,
) -> Result<ProcessOutput, IoError>
```

`Process.capture(program, arguments, input)` (type-system.md §26.7): runs `program`, found on `PATH`, with
`arguments`, `input` as its standard input (empty if null), and returns its exit status and what it wrote to
standard output and standard error. The streams go through files in a private temporary directory, removed
before this returns, so a program that writes much to both cannot block (ADR 0028).

### HostServices.kind

```ceesafe
public fn kind(path: StringView) -> Result<PathKind?, IoError>
```

`File.kind(path)` (type-system.md §26.8): what is at `path`, following symbolic links, or null if nothing is.
Safety: the runtime reads the path's bytes during the call only.

### HostServices.flush

```ceesafe
public fn flush()
```

`Console.flush()` (ADR 0077): writes what standard output holds buffered, so it survives the process being
killed. Standard error is never buffered.
Safety: the runtime only flushes the C library's standard output.

### HostServices.readLine

```ceesafe
public fn readLine(line: borrow mut String) -> Result<bool, IoError>
```

`Console.readLine(mut line)` (ADR 0106): reads the next line of standard input into `line`, replacing what it
held, without the line's ending (`\n`, or `\r\n`). `Ok(true)` with a line, which may be empty; `Ok(false)` at
the end of the input, with `line` empty. The input's last line needs no ending. Standard output is flushed
before the program waits for input, so a prompt is seen first.
Errors, each with `line` empty: `InvalidInput` for a line that is not UTF-8 (the line is consumed, and the
next call reads the one after it), `OutOfMemory`, or the read's error.
Standard input is read a block at a time; `Console.read` takes what a `readLine` left of its block first, so
the two can be mixed.
Safety: the runtime writes at most the chunk's length, during the call only.

### HostServices.modified

```ceesafe
public fn modified(path: StringView) -> Result<UnixTime, IoError>
```

`File.modified(path)` (ADR 0077): when the file or directory at `path` was last modified, as a `UnixTime`
(`stat`'s `st_mtime`, with nanoseconds where the system keeps them). The interpreter answers `Unsupported`.
Safety: the runtime reads the path's bytes and writes the two values during the call only.

### HostServices.setPermissions

```ceesafe
public fn setPermissions(path: StringView, mode: u32) -> Result<void, IoError>
```

`File.setPermissions(path, mode)` (ADR 0077): POSIX `chmod`, the low 12 bits of `mode` (`0o640`: the owner
reads and writes, the group reads). Windows has only a read-only attribute, set when the owner may not write.
A file created by `File.createNew` and given its permissions before anything is written to it never holds data
others could read. The interpreter answers `Unsupported`.
Safety: the runtime reads the path's bytes during the call only.

### HostServices.rename

```ceesafe
public fn rename(from: StringView, to: StringView) -> Result<void, IoError>
```

`File.rename(from, to)` (ADR 0075 §2.1): gives the file at `from` the name `to`, atomically replacing a file
there (POSIX `rename`; Windows `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`). The new
name is durable once its directory is synced (`Directory.sync`).
Safety: the runtime reads both paths' bytes during the call only.

## struct FileMetadata

```ceesafe
public struct FileMetadata
```

An open file's metadata (ADR 0078 §3), read from the file (`fstat`), not its path: it describes the file that is
read, even if another has been renamed into its place since it was opened. `f.modified()` and `f.kind()` are the
checker's names for FileMetadata.modified(f) and FileMetadata.kind(f).

### FileMetadata.modified

```ceesafe
public fn modified(file: borrow File) -> Result<UnixTime, IoError>
```

`f.modified()`: when the open file was last modified, with nanoseconds where the system keeps them. The
interpreter answers `Unsupported`, as for `File.modified(path)`.
Safety: the runtime reads the file's descriptor and writes the two values during the call only.

### FileMetadata.kind

```ceesafe
public fn kind(file: borrow File) -> Result<PathKind, IoError>
```

`f.kind()`: what the open file is. A directory opened for reading is `Directory`; a device, FIFO or socket is
`Other`.
Safety: the runtime reads the file's descriptor during the call only.

## struct UnixTime

```ceesafe
public struct UnixTime {
    public seconds: i64;
    public nanoseconds: u32;
}
```

A point in real time (ADR 0055): seconds since 1970-01-01T00:00:00Z, leap seconds not counted (POSIX time), negative
before it, and the nanoseconds after them.

### UnixTime.millis

```ceesafe
public fn millis(borrow this) -> i64
```

The milliseconds since 1970, rounded toward minus infinity (the nanoseconds are never negative).

## struct Clock

```ceesafe
public struct Clock
```

The system's real time (ADR 0055): the date, which may jump backward or forward (NTP, the user). Measure and wait
with `Timer`, whose monotonic clock never jumps. The interpreter answers 2000-01-01T00:00:00Z plus its virtual
clock, so its runs stay reproducible.

### Clock.now

```ceesafe
public fn now() -> UnixTime
```

Now.
Safety: the runtime writes the two values during the call only.

### Clock.unixMillis

```ceesafe
public fn unixMillis() -> i64
```

Now, in milliseconds since 1970.

### Clock.monotonicNanos

```ceesafe
public fn monotonicNanos() -> u64
```

Nanoseconds on the monotonic clock, from an arbitrary start (ADR 0075 §2.6): for measuring durations and
latencies, never for the date. It never decreases within a process (`CLOCK_MONOTONIC`; `CLOCK_UPTIME_RAW` on
Apple systems; `QueryPerformanceCounter` on Windows). The interpreter answers its virtual clock.
Safety: the runtime only reads the clock.

## struct Random

```ceesafe
public struct Random
```

Random bytes from the system's cryptographic source (ADR 0060): `getrandom` (or `/dev/urandom`) on Linux,
`arc4random_buf` on macOS and the BSDs, `BCryptGenRandom` on Windows; for keys, nonces, masks and tokens. Each call
reads the source, which is cheap enough to call per message. The interpreter answers a fixed sequence instead, so
its runs stay reproducible: its bytes are not secret.

### Random.fill

```ceesafe
public fn fill(into: SliceMut<u8>) -> Result<void, IoError>
```

Fills `into` with random bytes. `Err(Unsupported)` where the system has no source (CosmoOS userland); another
error if the source fails, with `into` partly written. It has no effect, so async code and a `drop` block may
call it.
Safety: the runtime writes at most the slice's length, during the call only.

### Random.below

```ceesafe
public fn below(n: u64) -> Result<u64, IoError>
```

A random integer `r` with `0 <= r < n`, each value equally likely (ADR 0106): `Random.below(100)! + 1` is a
number from 1 to 100. `Err(InvalidInput)` if `n` is 0, as no integer is below it; else `fill`'s error. It
draws eight bytes with `fill`, and again while they fall in the part of the 64-bit range that would make
some values likelier (less than one time in two, whatever `n`), so there is no modulo bias. In the
interpreter it follows `fill`'s fixed sequence: the same numbers every run.

## enum SocketAddress

```ceesafe
public enum SocketAddress {
    V4([u8; 4], u16),
    V6([u8; 16], u16, u32),
}
```

An IPv4 or IPv6 endpoint: the address bytes in network order and the port; an IPv6 address also has its scope id
(0: none), which a link-local address needs to say which interface it is on.

Implements `SocketEndpoint`.

## interface SocketEndpoint

```ceesafe
public interface SocketEndpoint {
    fn port(borrow this) -> u16;
    fn withPort(borrow this, p: u16) -> SocketAddress;
    fn isLoopback(borrow this) -> bool;
    fn isUnspecified(borrow this) -> bool;
    fn write(borrow this, w: borrow mut TextWriter);
}
```

What every SocketAddress answers (ADR 0044 §3.1).

## struct SocketAddresses

```ceesafe
public struct SocketAddresses
```

The ways to make a SocketAddress: from text, and the well-known ones. (An enum has no functions of its own,
type-system.md §15.1, so they are here.)

### SocketAddresses.parse

```ceesafe
public fn parse(text: StringView) -> SocketAddress?
```

`a.b.c.d:port`, `[v6]:port` or `[v6%zone]:port`, where the zone is a number or an interface's name; null if
the text is not one.

### SocketAddresses.parseIp

```ceesafe
public fn parseIp(text: StringView, port: u16) -> SocketAddress?
```

An address without a port (`127.0.0.1`, `::1`, `fe80::1%2`, `fe80::1%en0`) and `port`; null if the text is not
one.

### SocketAddresses.v4Any

```ceesafe
public fn v4Any(port: u16) -> SocketAddress
```

### SocketAddresses.v6Any

```ceesafe
public fn v6Any(port: u16) -> SocketAddress
```

### SocketAddresses.v4Loopback

```ceesafe
public fn v4Loopback(port: u16) -> SocketAddress
```

### SocketAddresses.v6Loopback

```ceesafe
public fn v6Loopback(port: u16) -> SocketAddress
```

## struct ListenOptions

```ceesafe
public struct ListenOptions {
    public backlog: u32;
    public reuseAddress: bool;
    public reusePort: bool;
    public v6Only: bool;
}
```

How a listener is made (ADR 0044 §3.2). `defaults()`: the platform's largest backlog, the address reusable (as
the runtime always did), no port sharing, and dual stack for an IPv6 address.

### ListenOptions.defaults

```ceesafe
public fn defaults() -> ListenOptions
```

## struct IoParts

```ceesafe
public struct IoParts<region r>
```

A list of up to eight byte parts sent as one (ADR 0045 §3.1): an HTTP response's head and body, or a chunk's size
line, data and CRLF, in one system call and without a copy. It is a value built by copying, since a slice cannot be
an array element: `IoParts.of(head).then(body)`. Empty parts are allowed and send nothing.

### IoParts.of

```ceesafe
public fn of(part: Slice<r, u8>) -> IoParts<r>
```

One part.

### IoParts.then

```ceesafe
public fn then(this, part: Slice<r, u8>) -> IoParts<r>?
```

These parts and `part` after them, or null when there are eight already.

### IoParts.count

```ceesafe
public fn count(borrow this) -> u32
```

How many parts (0 to 8; 0 only from `after`).

### IoParts.part

```ceesafe
public fn part(this, i: u32) -> Slice<r, u8>
```

Part `i` (from 0); empty past the last.

### IoParts.totalLength

```ceesafe
public fn totalLength(borrow this) -> usize
```

The number of bytes in every part.

### IoParts.after

```ceesafe
public fn after(this, skip: usize) -> IoParts<r>
```

The bytes after the first `skip`, as parts: whole parts dropped, the part where byte `skip` falls cut. Empty
(no parts) from `totalLength()` on. What a write loop sends after a partial send.

## struct UdpOptions

```ceesafe
public struct UdpOptions {
    public reuseAddress: bool;
    public reusePort: bool;
    public v6Only: bool;
    public sendBuffer: u32;
    public recvBuffer: u32;
    public receiveEcn: bool;
    public receiveLocal: bool;
}
```

How a UDP socket is made (ADR 0050 §3.1). `defaults()`: no option set, the system's buffer sizes, and dual stack for
an IPv6 address.

### UdpOptions.defaults

```ceesafe
public fn defaults() -> UdpOptions
```

## struct Datagram

```ceesafe
public struct Datagram {
    public length: usize;
    public from: SocketAddress;
    public truncated: bool;
}
```

What `AsyncUdpSocket.recvFrom` gives: the datagram's length in the buffer, where it came from, and whether it was
longer than the buffer (the rest is gone).

## struct UdpReceived

```ceesafe
public struct UdpReceived {
    public length: usize;
    public truncated: bool;
    public from: SocketAddress;
    public to: SocketAddress?;
    public ecn: u8;
}
```

What `AsyncUdpSocket.recvMsg` gives (ADR 0050 §3.1).

## struct UdpMessage

```ceesafe
public struct UdpMessage<region r> {
    public parts: IoParts<r>;
    public to: SocketAddress;
    public from: SocketAddress?;
    public ecn: u8;
    public segmentSize: u16;
}
```

A datagram for `AsyncUdpSocket.sendMsg` (ADR 0050 §3.1): its parts (ADR 0045), where it goes, and optionally the
local address it leaves from, its ECN bits and a segment size.

## struct AsyncDns

```ceesafe
public struct AsyncDns
```

Name resolution for tasks (ADR 0048).

### AsyncDns.resolve

```ceesafe
public async fn resolve(host: StringView, port: u16) -> Result<Vector<SocketAddress>, AsyncError>
```

The addresses of `host`, each with `port`: both families, in the resolver's order, duplicates removed, never
empty. A literal address is itself, at once. Otherwise the system's resolver runs on one of at most four
resolver threads while the task waits; a cancelled task returns `Err(Cancelled)` at once, and the lookup is left
to its thread. Errors are `AsyncError.Io`: `HostNotFound` (no such name, or no address for it; ADR 0060),
`TimedOut` (a temporary failure: try again), `InvalidInput` (an empty name, one over 253 bytes or with a NUL),
`OutOfMemory`, `Unsupported` (no resolver: CosmoOS, the interpreter for names other than `localhost` and the
reserved `.invalid` ones).
Safety: the runtime copies the name during the start; the lookup's record is the runtime's, ended once by
`DnsLookup`'s drop, so no path leaves it or ends it twice.

## interface OrderKey

```ceesafe
public interface OrderKey<K> {
    fn compareKey(borrow this, other: borrow K) -> i32;
}
```

A key of an OrderedMap (ADR 0075 §2.4): `K: OrderKey<K>`, since an interface has no `Self`. The order must be total
and consistent: `a.compareKey(b)` is negative, zero or positive as `b.compareKey(a)` is positive, zero or negative.

## fn deriveCompare

```ceesafe
public fn deriveCompare<K: OrderKey<K>>(a: borrow K, b: borrow K) -> i32
```

`a.compareKey(b)`: what a derived `compareKey` does for each field (ADR 0087 §2.3). A field type without an
`OrderKey` is N0268 at the `@derive`.

## struct OrderedCursor

```ceesafe
public struct OrderedCursor
```

A position in an OrderedMap: a node, a slot in it, and the map's version when the cursor was made. A cursor is
read through its map (`keyAt`, `valueAt`); one used after the map changed panics.

## struct OrderedMap

```ceesafe
public struct OrderedMap<K: OrderKey<K>, V>
```

A map whose iteration order is its keys' order (ADR 0075 §2.4): a B-tree of minimum degree 16 (at most 31 entries
a node), its nodes in one arena vector and linked by index, so it needs no `unsafe`. Ordered access is through
cursors (`first`, `last`, `seek`, `seekAfter`, `next`, `previous`); a put, a removal or a clear changes the map's
version, and a cursor made before it panics when used ("ordered map changed under a cursor"). Only `put`
allocates; a failed `put` leaves the map as it was.

### OrderedMap.create

```ceesafe
public fn create() -> OrderedMap<K, V>
```

An empty map. It allocates nothing until the first put.

### OrderedMap.length

```ceesafe
public fn length(borrow this) -> usize
```

How many entries.

### OrderedMap.isEmpty

```ceesafe
public fn isEmpty(borrow this) -> bool
```

Whether there are none.

### OrderedMap.get

```ceesafe
public fn get(borrow this, key: borrow K) -> borrow(this) V?
```

The value of `key`, if it is there.

### OrderedMap.getMut

```ceesafe
public fn getMut(borrow mut this, key: borrow K) -> borrow mut(this) V?
```

The same, to change it. Changing a value does not change the map's version.

### OrderedMap.contains

```ceesafe
public fn contains(borrow this, key: borrow K) -> bool
```

Whether `key` is there.

### OrderedMap.put

```ceesafe
public fn put(borrow mut this, key: K, value: V) -> Result<V?, AllocError>
```

Puts `value` at `key`: the old value if the key was there, else null. A failure leaves the map as it was. The
nodes a put may need are allocated before anything changes.

### OrderedMap.remove

```ceesafe
public fn remove(borrow mut this, key: borrow K) -> V?
```

Takes `key` out: its value, or null. The descent keeps every node it enters above the minimum, borrowing from a
sibling or else merging (CLRS's B-TREE-DELETE), so the tree stays balanced. Nothing allocates.

### OrderedMap.clear

```ceesafe
public fn clear(borrow mut this)
```

Takes every entry out and frees the nodes.

### OrderedMap.first

```ceesafe
public fn first(borrow this) -> OrderedCursor?
```

The least entry, or null if the map is empty.

### OrderedMap.last

```ceesafe
public fn last(borrow this) -> OrderedCursor?
```

The greatest entry, or null if the map is empty.

### OrderedMap.seek

```ceesafe
public fn seek(borrow this, key: borrow K) -> OrderedCursor?
```

The first entry whose key is not before `key`, or null if there is none.

### OrderedMap.seekAfter

```ceesafe
public fn seekAfter(borrow this, key: borrow K) -> OrderedCursor?
```

The first entry whose key comes after `key`, or null if there is none.

### OrderedMap.next

```ceesafe
public fn next(borrow this, at: OrderedCursor) -> OrderedCursor?
```

The entry after `at`'s, or null at the end.

### OrderedMap.previous

```ceesafe
public fn previous(borrow this, at: OrderedCursor) -> OrderedCursor?
```

The entry before `at`'s, or null at the start.

### OrderedMap.keyAt

```ceesafe
public fn keyAt(borrow this, at: OrderedCursor) -> borrow(this) K
```

The key at `at`.

### OrderedMap.valueAt

```ceesafe
public fn valueAt(borrow this, at: OrderedCursor) -> borrow(this) V
```

The value at `at`.

### OrderedMap.valueAtMut

```ceesafe
public fn valueAtMut(borrow mut this, at: OrderedCursor) -> borrow mut(this) V
```

The value at `at`, to change it. Changing a value does not change the map's version.

## struct Mutex

```ceesafe
@threadSafe
@noncopy
public struct Mutex<T: Send> {
    drop { … }
}
```

Mutual exclusion: the data is reached only through a guard, which one thread holds at a time.

### Mutex.create

```ceesafe
public fn create(data: T) -> Result<Mutex<T>, AllocError>
```

A mutex holding `data`; its platform lock is allocated.
Safety: a null lock is reported, never stored.

### Mutex.lock

```ceesafe
public fn lock(borrow this) -> MutexGuard<T>
```

Waits for the lock; the guard lends the data until it is dropped.
Safety: `raw` is the lock made in create, destroyed only in drop, when no guard can be live (B-rules).

### Mutex.tryLock

```ceesafe
public fn tryLock(borrow this) -> MutexGuard<T>?
```

The guard if no thread holds the lock, else null, at once: it never waits (ADR 0112). The thread that holds
the lock gets null too. It has no effect, so async code may call it.
Safety: as `lock`; the guard exists only when the lock was taken.

## struct MutexGuard

```ceesafe
public struct MutexGuard<region r, T: Send> {
    drop { … }
}
```

Holding a Mutex: `data()` lends the protected value; dropping the guard unlocks.

### MutexGuard.data

```ceesafe
public fn data(borrow mut this) -> borrow mut T
```

Safety: the guard holds the lock, so no other thread reaches the cell's contents, and the loan ends before the
guard's drop (a RawCell's contents may change while the mutex is shared, §41.1).

## struct Arc

```ceesafe
@threadSafe
@noncopy
public struct Arc<T: Share> {
    drop { … }
}
```

Shared ownership across threads: each clone is an owner, and the last owner's drop destroys the value.

### Arc.create

```ceesafe
public fn create(value: T) -> Result<Arc<T>, AllocError>
```

Safety: the pointer comes from Box.intoRaw and is given back once, by the last owner's drop.

### Arc.clone

```ceesafe
public fn clone(borrow this) -> Arc<T>
```

Another owner of the same value.
Safety: this owner keeps the value alive while the count is raised.

### Arc.get

```ceesafe
public fn get(borrow this) -> borrow T
```

The shared value, for as long as this owner is borrowed.
Safety: the value lives while any owner does; T is Share, so shared references to it may cross threads.

## enum SpawnError

```ceesafe
public enum SpawnError {
    Resources,
}
```

Why Thread.spawn failed: the thread, its record or its closure could not be allocated (type-system.md §38.2).

## struct OnceLock

```ceesafe
@threadSafe
@noncopy
public struct OnceLock<T: Send + Share>
```

A value written at most once and then read from any thread (§38.3, ADR 0096): `OnceCell` (lib/core, §41.4) for
values that threads share. Empty at first, then full for as long as the lock is shared, lending `borrow T`. One
thread runs an initialiser at a time, under one lock for the process; a thread that finds a full cell reads it
with one atomic load and takes no lock. A full cell changes only through an exclusive borrow (`take`, `getMut`).
The state is one byte: 0 empty, 1 the initialiser is running, 2 full.

### OnceLock.create

```ceesafe
public fn create() -> OnceLock<T>
```

An empty cell.

### OnceLock.get

```ceesafe
public fn get(borrow this) -> borrow T?
```

The value, or null while the cell is empty.

### OnceLock.getOrInit

```ceesafe
public fn getOrInit(borrow this, init: borrow mut fn() -> T) -> borrow(this) T
```

The value; an empty cell runs `init` and stores its result first. Threads that arrive while another runs
`init` wait for it and read its value. Panics if `init` asks this cell for its value ("cell initialised
recursively").
Safety: the write happens under the lock with the state 0 seen there, so no reference to the contents has
been lent and no other thread writes; the old contents are null and need no drop. The lock is released on
every path.

### OnceLock.tryInit

```ceesafe
public fn tryInit<E>(borrow this, init: borrow mut fn() -> Result<T, E>) -> Result<void, E>
```

Fills an empty cell from an initialiser that can fail: on `Err` the cell stays empty and the error is
returned. A full cell is left as it is, and `init` does not run. `get` or `getOrInit` then lends the value.
Panics as getOrInit.
Safety: as getOrInit.

### OnceLock.set

```ceesafe
public fn set(borrow this, value: T) -> Result<void, T>
```

Stores `value` in an empty cell. A cell that is full, or whose initialiser is running, gives the value back.
Safety: as getOrInit.

### OnceLock.getMut

```ceesafe
public fn getMut(borrow mut this) -> (borrow mut T)?
```

The value, for as long as the cell is borrowed exclusively; null while the cell is empty.

### OnceLock.take

```ceesafe
public fn take(borrow mut this) -> T?
```

The value, leaving the cell empty: no reference to it can be live while the cell is borrowed exclusively.
Safety: the contents are read out and null written in their place, so they have one owner.

### OnceLock.intoInner

```ceesafe
public fn intoInner(this) -> T?
```

The value, the cell consumed.

## struct TestFailure

```ceesafe
public struct TestFailure {
    public message: Vector<u8>;
}
```

What a failed check returns (ADR 0049 §3.2): a message, owned, so it borrows nothing from the test. `ceesafec test`
shows it with the test's name.

### TestFailure.create

```ceesafe
public fn create(message: StringView) -> TestFailure
```

A failure with `message`. If the message cannot be allocated, the failure has an empty one: it still fails.

## struct Check

```ceesafe
public struct Check
```

The checks of a test (ADR 0049 §3.2): each is `Ok` when it holds, and otherwise `Err` with what was expected, for
`?`. The comparisons are typed: CeeSafe has no equality interface yet for a generic one.

### Check.isTrue

```ceesafe
public fn isTrue(condition: bool, what: StringView) -> Result<void, TestFailure>
```

`condition` holds; `what` says what it is.

### Check.isFalse

```ceesafe
public fn isFalse(condition: bool, what: StringView) -> Result<void, TestFailure>
```

`condition` does not hold.

### Check.equalU64

```ceesafe
public fn equalU64(expected: u64, actual: u64) -> Result<void, TestFailure>
```

`actual` is `expected`.

### Check.equalI64

```ceesafe
public fn equalI64(expected: i64, actual: i64) -> Result<void, TestFailure>
```

`actual` is `expected`.

### Check.equalText

```ceesafe
public fn equalText(expected: StringView, actual: StringView) -> Result<void, TestFailure>
```

`actual` is the text `expected`.

### Check.equalBytes

```ceesafe
public fn equalBytes(expected: Slice<u8>, actual: Slice<u8>) -> Result<void, TestFailure>
```

`actual` has the bytes of `expected`; else where they first differ.

### Check.fail

```ceesafe
public fn fail(message: StringView) -> Result<void, TestFailure>
```

A failure with `message`, always.

## struct CheckResult

```ceesafe
public struct CheckResult<T, E>
```

The checks of a `Result` (ADR 0049 §3.2), a generic struct's since a method declares no type parameters of its own:
`CheckResult<u32, ParseError>.isOk(borrow parsed, "the line parses")?`.

### CheckResult.isOk

```ceesafe
public fn isOk(r: borrow Result<T, E>, what: StringView) -> Result<void, TestFailure>
```

`r` is `Ok`.

### CheckResult.isErr

```ceesafe
public fn isErr(r: borrow Result<T, E>, what: StringView) -> Result<void, TestFailure>
```

`r` is `Err`.

## struct TestRun

```ceesafe
public struct TestRun
```

The generated harness's side of the runner protocol, version 1 (ADR 0049 §3.4). Not for tests to call.

### TestRun.begin

```ceesafe
public fn begin()
```

Before the test: the arguments after the test's name, `<fail-alloc-at> <cancel-at> <timeout-ms>` (0: none),
turn the runtime's reports and hooks on and arm the timeout.
Safety: the runtime only records the numbers, and the watchdog only waits and ends the process.

### TestRun.passed

```ceesafe
public fn passed() -> i32
```

The test passed: its line, and the exit status 0.

### TestRun.failed

```ceesafe
public fn failed(message: Slice<u8>) -> i32
```

The test failed with `message`: its lines, and the exit status 1.

### TestRun.failedWith

```ceesafe
public fn failedWith(message: StringView) -> i32
```

The same with a fixed message.

### TestRun.unknown

```ceesafe
public fn unknown() -> i32
```

No test has the name the runner asked for.

## enum WindowError

```ceesafe
public enum WindowError {
    Unsupported,
    NoDisplay,
    NotMainThread,
    Resources,
}
```

Why a window could not be opened or drawn (§26.13).

## struct WindowSize

```ceesafe
public struct WindowSize {
    public width: u32;
    public height: u32;
}
```

A window's content size in points.

## struct WindowBox

```ceesafe
public struct WindowBox {
    public x: u32;
    public y: u32;
    public width: u32;
    public height: u32;
}
```

A part of an image that changed (`Window.presentChanged`), in the image's pixels.

## enum WindowButton

```ceesafe
public enum WindowButton {
    Primary,
    Secondary,
    Middle,
}
```

A pointer button.

## struct WindowModifiers

```ceesafe
public struct WindowModifiers {
    public shift: bool;
    public control: bool;
    public alt: bool;
    public command: bool;
}
```

The modifier keys held during a key event. `command` is ⌘ on macOS and the Windows key elsewhere.

## enum WindowKey

```ceesafe
public enum WindowKey {
    Tab,
    Enter,
    Escape,
    Backspace,
    Delete,
    Left,
    Right,
    Up,
    Down,
    Home,
    End,
    PageUp,
    PageDown,
    Space,
    Character(u32),
    Function(u8),
    Other(u32),
}
```

A key (§26.13): the named keys a UI needs, letters and digits as `Character`, F1 to F12 as `Function`.

## enum WindowCursor

```ceesafe
public enum WindowCursor {
    Default,
    Pointer,
    Text,
    Crosshair,
    Grab,
    ResizeHorizontal,
    ResizeVertical,
}
```

The pointer's shape over the content.

## enum WindowEvent

```ceesafe
public enum WindowEvent {
    Closed,
    Resized(WindowSize),
    Focus(bool),
    PointerMoved(f32, f32),
    PointerButton(WindowButton, bool, f32, f32),
    Wheel(f32, f32),
    Key(WindowKey, bool, bool, WindowModifiers),
    Text(u32),
}
```

What happened to a window (§26.13). Positions are points from the content's top left.

## struct Window

```ceesafe
public struct Window {
    drop { … }
}
```

A native window (§26.13): a move type that closes the window when dropped. It is used on the program's first
thread only.

### Window.open

```ceesafe
public fn open(title: StringView, width: u32, height: u32) -> Result<Window, WindowError>
```

Opens a resizable window of `width` × `height` points titled `title`.
Safety: the runtime reads the title's bytes during the call only, and the handle it returns is this window's
until the drop closes it.

### Window.nextEvent

```ceesafe
public fn nextEvent(borrow mut this) -> WindowEvent?
```

The next pending event, or null; it never blocks.
Safety: the runtime writes four fields and two coordinates, the sizes of the arrays it is given.

### Window.present

```ceesafe
public fn present(borrow mut this, pixels: Slice<u32>, width: u32, height: u32) -> Result<void, WindowError>
```

Shows a `width` × `height` image of premultiplied RGBA8 pixels (bytes R, G, B, A), rows top to bottom, scaled
to the content.
Safety: the runtime reads `width × height` pixels, which `pixels` is checked to hold, during the call only.

### Window.presentChanged

```ceesafe
public fn presentChanged(
    borrow mut this,
    pixels: Slice<u32>,
    width: u32,
    height: u32,
    changed: Slice<WindowBox>,
) -> Result<void, WindowError>
```

Like `present`, but only the pixels inside `changed` differ from the last frame presented (§26.13, ADR 0070):
the window shows just those, which saves copying the rest. Without a last frame of this size, with a box
outside the image, or with more than 64 boxes, it is `present`. An empty `changed` shows nothing new.
Safety: the runtime reads `width × height` pixels, which `pixels` is checked to hold, and the boxes, during
the call only.

### Window.size

```ceesafe
public fn size(borrow this) -> WindowSize
```

The content's size in points.
Safety: the runtime writes the two sizes.

### Window.scale

```ceesafe
public fn scale(borrow this) -> f32
```

Pixels per point: 2.0 on a Retina display.
Safety: the runtime writes the scale.

### Window.setCursor

```ceesafe
public fn setCursor(borrow mut this, cursor: WindowCursor)
```

The pointer's shape while it is over the content.
Safety: the runtime only reads the cursor's number.

### Window.setTitle

```ceesafe
public fn setTitle(borrow mut this, title: StringView)
```

Sets the title.
Safety: the runtime reads the title's bytes during the call only.

### Window.waitFrame

```ceesafe
public fn waitFrame(borrow mut this)
```

Returns about 1/60 s after the previous call, or earlier when an event arrives.
Safety: the runtime only waits on the window it was given.
