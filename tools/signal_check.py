#!/usr/bin/env python3
"""A program is told to end by a signal, and ends in good order (ADR 0125, ADR 0126): checks the corpus cannot make,
since its runner sends a program no signal.

Each case starts a program, built and with `ceesafec run --interp`, sends it SIGINT or SIGTERM with os.kill, and
checks what it writes and how it ends, with a deadline for every wait:

1. a server of tasks (`Async.race` of the accepting loop and `signals.waitAsync()`, a shield around each
   connection): a client has sent half its request when the signal comes; the server stops accepting, the client
   sends the rest and gets its whole answer, and the server exits 0. Built, it runs with each reactor the host has
   (the default, CEESAFE_REACTOR_POLL=1, and CEESAFE_REACTOR_URING=1 on Linux);
2. a server of threads (the main thread waits in `signals.wait()` and calls `listener.shutdown()`; the accepting
   thread's `accept` returns `ConnectionAborted`, and it joins the connections' threads): the same dialogue, built.
   The interpreter runs one thread at a time, so there the server is only told to stop, with no client;
3. a program that takes one signal with `wait`, finds no other with `poll`, drops its subscription and waits for a
   line: the first signal is named, and the second one ends the process as it would have without a subscription;
4. a program that never subscribes is ended by the signal, as before ADR 0125.

POSIX only (os.kill with SIGINT and SIGTERM): elsewhere it reports that it was skipped. On Windows the same
programs are tried by hand: Ctrl+C in a console (tools/signal_check.py --write DIR writes them).

usage: python3 tools/signal_check.py --compiler out/ceesafec
"""

import argparse
import os
import select
import signal
import socket
import subprocess
import sys
import tempfile
import time

HANDLER = """
/// Reads a request's head, then answers.
async fn handle(stream: borrow AsyncTcpStream) -> Result<void, ServeError> {
    var head = Vector<u8>.create();
    var buffer = [0u8; 256];
    loop {
        let n = await stream.read(mut buffer[..])?;
        if (n == 0) {
            break;
        }
        head.extend(buffer[0..n])?;
        if (head.view().contains("\\r\\n\\r\\n".bytes())) {
            break;
        }
    }
    await stream.write("HTTP/1.1 200 OK\\r\\nContent-Length: 2\\r\\nConnection: close\\r\\n\\r\\nok".bytes())?;
    stream.shutdownWrite()?;
    return Ok();
}
"""

TASKS = """
enum ServeError {
    NoMemory,
    @from Io(IoError),
    @from Async(AsyncError),
    @from Memory(AllocError),
}
""" + HANDLER + """
/// One connection's task: the shield lets its answer finish while the server stops.
async fn connection(stream: AsyncTcpStream) -> Result<void, ServeError> {
    let result = await Async.shield(15000, async () => await handle(borrow stream));
    if (result.isErr()) {
        await AsyncConsole.writeError("a connection failed\\n")?;
    }
    return Ok();
}

async fn acceptLoop(listener: borrow AsyncTcpListener, set: borrow TaskSet<ServeError>) -> Result<void, ServeError> {
    loop {
        let stream = await listener.accept()?;
        await set.spawn(async move () => await connection(stream))?;
    }
}

async fn serverSide(listener: borrow AsyncTcpListener) -> Result<bool, ServeError> {
    await Async.tasks(16, ServeError.NoMemory, async (set) => await acceptLoop(listener, set))?;
    return Ok(false);
}

async fn stopAsked(signals: borrow ProcessSignals) -> Result<bool, ServeError> {
    let asked = await signals.waitAsync()?;
    await AsyncConsole.writeLine(if (asked is ProcessSignal.Interrupt) "interrupt" else "terminate")?;
    return Ok(true);
}

async fn serveUntilStopped(listener: borrow AsyncTcpListener, signals: borrow ProcessSignals) -> Result<void, ServeError> {
    await Async.race(async () => await serverSide(listener), async () => await stopAsked(signals))?;
    return Ok();
}

fn main() {
    let listener = AsyncTcpListener.bind("127.0.0.1", 0)!;
    let signals = ProcessSignals.subscribe()!;
    Console.write("listening ");
    Console.writeLine(listener.port()!);
    Console.flush();
    match (Async.run(serveUntilStopped(borrow listener, borrow signals))) {
        Ok() => { Console.writeLine("stopped"); }
        Err(_) => {
            Console.writeErrorLine("the server failed");
            Process.exit(1);
        }
    }
}
"""

THREADS = """
/// Reads a request's head, then answers.
fn handle(stream: borrow mut TcpStream) -> Result<void, IoError> {
    var head = Vector<u8>.create();
    var buffer = [0u8; 256];
    loop {
        let n = stream.read(mut buffer[..])?;
        if (n == 0) {
            break;
        }
        if (head.extend(buffer[0..n]).isErr()) {
            return Err(IoError.OutOfMemory);
        }
        if (head.view().contains("\\r\\n\\r\\n".bytes())) {
            break;
        }
    }
    stream.write("HTTP/1.1 200 OK\\r\\nContent-Length: 2\\r\\nConnection: close\\r\\n\\r\\nok".bytes())?;
    stream.shutdownWrite()?;
    return Ok();
}

fn main() {
    let listener = Arc<TcpListener>.create(TcpListener.bind("127.0.0.1", 0)!)!;
    let signals = ProcessSignals.subscribe()!;
    Console.write("listening ");
    Console.writeLine(listener.get().port()!);
    Console.flush();
    // The accepting thread: a thread for each connection, all joined once the listener is shut down.
    let accepting = listener.clone();
    let acceptor = Thread.spawn(move () => {
        var connections = Vector<Thread<bool>>.create();
        loop {
            match (accepting.get().accept()) {
                Ok(accepted) => {
                    connections.push(Thread.spawn(move () => {
                        var stream = accepted;
                        return handle(mut stream).isOk();
                    })!)!;
                }
                Err(IoError.ConnectionAborted) => { break; } // the listener was shut down
                Err(_) => {
                    Console.writeErrorLine("accept failed");
                    break;
                }
            }
        }
        // No new connection is accepted; the ones in flight are answered.
        var answered: u32 = 0;
        while (connections.pop() is Some(thread)) {
            if (thread.join()) {
                answered += 1;
            }
        }
        return answered;
    })!;
    // The main thread waits for the request to stop, and then ends the other thread's `accept`.
    let asked = signals.wait();
    Console.writeLine(if (asked is ProcessSignal.Interrupt) "interrupt" else "terminate");
    Console.flush();
    listener.get().shutdown()!;
    let answered = acceptor.join();
    Console.write("stopped after ");
    Console.writeLine(answered);
}
"""

ONCE = """
fn main() {
    let signals = ProcessSignals.subscribe()!;
    Console.writeLine("ready");
    Console.flush();
    let asked = signals.wait();
    Console.writeLine(if (asked is ProcessSignal.Interrupt) "interrupt" else "terminate");
    Console.writeLine(if (signals.poll() is null) "no other" else "another");
    {
        let ended = signals; // the subscription ends with this block
    }
    Console.writeLine("dropped");
    Console.flush();
    var line = String.create();
    let read = Console.readLine(mut line);
    Console.writeLine("a line?");
}
"""

PLAIN = """
fn main() {
    Console.writeLine("ready");
    Console.flush();
    var line = String.create();
    let read = Console.readLine(mut line);
    Console.writeLine("a line?");
}
"""

PROGRAMS = (("tasks", TASKS), ("threads", THREADS), ("once", ONCE), ("plain", PLAIN))
DEADLINE = 30.0


class Reader:
    """Reads a descriptor up to a marker, with a deadline."""

    def __init__(self, fd):
        self.fd, self.seen = fd, b""

    def until(self, marker):
        end = time.monotonic() + DEADLINE
        while marker not in self.seen:
            left = end - time.monotonic()
            ready = select.select([self.fd], [], [], max(left, 0))[0] if left > 0 else []
            if not ready:
                return None
            piece = os.read(self.fd, 4096)
            if not piece:
                return None
            self.seen += piece
        at = self.seen.index(marker) + len(marker)
        text, self.seen = self.seen[:at], self.seen[at:]
        return text


def start(argv, env):
    return subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            env=dict(os.environ, **env))


def finish(p):
    p.kill()
    p.wait()
    for stream in (p.stdin, p.stdout, p.stderr):
        try:
            stream.close()
        except OSError:
            pass


def server(argv, env, problems, name, number, word, client, last):
    """A server is told to stop with a request half sent (`client`), or with no client at all."""
    p = start(argv, env)
    out = Reader(p.stdout.fileno())
    try:
        line = out.until(b"\n")
        if line is None or not line.startswith(b"listening "):
            problems.append(f"{name}: no 'listening PORT' (seen {out.seen!r}, {p.stderr.read1(400)!r})")
            return
        port = int(line.split()[1])
        slow = None
        if client:
            slow = socket.create_connection(("127.0.0.1", port), timeout=DEADLINE)
            slow.sendall(b"GET /slow HT")
            # A whole request on a second connection: when it is answered, the first one is accepted too.
            with socket.create_connection(("127.0.0.1", port), timeout=DEADLINE) as quick:
                quick.sendall(b"GET / HTTP/1.1\r\n\r\n")
                if not quick.recv(4096).startswith(b"HTTP/1.1 200 OK"):
                    problems.append(f"{name}: the first request was not answered")
                    return
        os.kill(p.pid, number)
        if slow is not None:
            # The server has stopped accepting when a whole new request gets no answer. (What it wrote is read at
            # the end: the interpreter keeps a task's output until the program flushes or ends.)
            time.sleep(0.5)
            if p.poll() is not None:
                problems.append(f"{name}: the server ended (status {p.returncode}) with a request in flight")
                return
            late = socket.create_connection(("127.0.0.1", port), timeout=DEADLINE)
            late.sendall(b"GET /late HTTP/1.1\r\n\r\n")
            late.settimeout(1.0)
            try:
                answer = late.recv(4096)
            except (socket.timeout, ConnectionError):
                answer = b""
            if answer.startswith(b"HTTP"):
                problems.append(f"{name}: a connection made after the signal was answered")
                return
            slow.sendall(b"TP/1.1\r\n\r\n")
            reply = b""
            while True:
                piece = slow.recv(4096)
                if not piece:
                    break
                reply += piece
            slow.close()
            late.close()
            if not (reply.startswith(b"HTTP/1.1 200 OK") and reply.endswith(b"ok")):
                problems.append(f"{name}: the request in flight was not answered in full (got {reply!r})")
                return
        expected = word + b"\n" + last
        if out.until(last) != expected or p.wait(DEADLINE) != 0:
            problems.append(f"{name}: no {expected!r} and exit 0 after the signal (seen {out.seen!r}, "
                            f"status {p.poll()}, {p.stderr.read1(400)!r})")
    except (OSError, ValueError, subprocess.TimeoutExpired) as e:
        problems.append(f"{name}: {e}")
    finally:
        finish(p)


def once(argv, problems, name, first, word):
    """One signal is taken; after the subscription's drop, the next one ends the process."""
    p = start(argv, {})
    out = Reader(p.stdout.fileno())
    try:
        if out.until(b"ready\n") is None:
            problems.append(f"{name}: no 'ready' (seen {out.seen!r}, {p.stderr.read1(400)!r})")
            return
        time.sleep(0.2)  # the program reaches its wait
        os.kill(p.pid, first)
        expected = word + b"\nno other\ndropped\n"
        if out.until(b"dropped\n") != expected:
            problems.append(f"{name}: expected {expected!r} after the first signal (seen {out.seen!r}, "
                            f"status {p.poll()})")
            return
        time.sleep(0.2)
        os.kill(p.pid, signal.SIGINT)
        status = p.wait(DEADLINE)
        if status != -signal.SIGINT or out.until(b"a line?") is not None:
            problems.append(f"{name}: after the drop, SIGINT did not end the process (status {status}, "
                            f"seen {out.seen!r})")
    except (OSError, subprocess.TimeoutExpired) as e:
        problems.append(f"{name}: {e}")
    finally:
        finish(p)


def plain(argv, problems, name, number):
    """Without a subscription a signal ends the process, as it always did."""
    p = start(argv, {})
    out = Reader(p.stdout.fileno())
    try:
        if out.until(b"ready\n") is None:
            problems.append(f"{name}: no 'ready' (seen {out.seen!r}, {p.stderr.read1(400)!r})")
            return
        os.kill(p.pid, number)
        status = p.wait(DEADLINE)
        if status != -number:
            problems.append(f"{name}: expected the signal to end the process (status {status})")
    except (OSError, subprocess.TimeoutExpired) as e:
        problems.append(f"{name}: {e}")
    finally:
        finish(p)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--write", metavar="DIR", help="only write the programs to DIR (to try them by hand)")
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    if args.write:
        os.makedirs(args.write, exist_ok=True)
        for stem, source in PROGRAMS:
            with open(os.path.join(args.write, stem + ".cee"), "w", encoding="utf-8") as f:
                f.write(source)
        print(f"signal_check: wrote {len(PROGRAMS)} programs to {args.write}")
        return 0
    if os.name != "posix":
        print("signal_check: skipped (os.kill with SIGINT and SIGTERM is POSIX's)")
        return 0
    problems = []
    cases = 0
    signals = ((signal.SIGINT, b"interrupt"), (signal.SIGTERM, b"terminate"))
    reactors = [("", {}), (", poll", {"CEESAFE_REACTOR_POLL": "1"})]
    if sys.platform.startswith("linux"):
        reactors.append((", io_uring", {"CEESAFE_REACTOR_URING": "1"}))
    with tempfile.TemporaryDirectory(prefix="ceesafe-signal-") as work:
        programs = {}
        for stem, source in PROGRAMS:
            path = os.path.join(work, stem + ".cee")
            with open(path, "w", encoding="utf-8") as f:
                f.write(source)
            exe = os.path.join(work, stem)
            b = subprocess.run([compiler, "build", path, "-o", exe], capture_output=True, text=True)
            if b.returncode != 0:
                problems.append(f"{stem}: build failed: {b.stderr.strip()[:600]}")
                continue
            programs[stem] = {"native": [exe], "interp": [compiler, "run", "--interp", path]}
        for mode, argv in programs.get("tasks", {}).items():
            for number, word in signals:
                for label, env in (reactors if mode == "native" else reactors[:1]):
                    server(argv, env, problems, f"tasks ({mode}{label}, {word.decode()})", number, word, True,
                           b"stopped\n")
                    cases += 1
        for mode, argv in programs.get("threads", {}).items():
            for number, word in signals:
                # The interpreter runs one thread at a time: its server is told to stop before any client comes.
                client = mode == "native"
                server(argv, {}, problems, f"threads ({mode}, {word.decode()})", number, word, client,
                       b"stopped after 2\n" if client else b"stopped after 0\n")
                cases += 1
        for mode, argv in programs.get("once", {}).items():
            for number, word in signals:
                once(argv, problems, f"one signal, then the default ({mode}, {word.decode()})", number, word)
                cases += 1
        for mode, argv in programs.get("plain", {}).items():
            for number, word in signals:
                plain(argv, problems, f"no subscription ({mode}, {word.decode()})", number)
                cases += 1
    for problem in problems:
        print(f"FAIL {problem}")
    print(f"signal_check: {cases} cases: {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
