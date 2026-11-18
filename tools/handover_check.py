#!/usr/bin/env python3
"""What a child is handed (ADR 0173): checks the corpus cannot make, since they need several processes.

One program is built and run in several ways:

1. `parent`: binds a listener on a free port and makes memory with no name, lists both in a `Handover`, starts
   itself three times as `worker N` with it (`Process.spawnWith`), and drops its own listener. This script makes 300
   connections to the port, each answered by whichever worker accepts it; the workers count in the memory, and the
   counts sum to 300. Worker 0 also checks the places: their kinds, a place of the other kind, a place taken
   twice and a place past the end (`NotFound`), and that a program it starts with `Process.spawn` is handed
   nothing. The parent then ends the workers and binds the port again: nothing of the socket is left;
2. `datagrams`: a UDP socket handed to two children, each of which receives one datagram this script sends;
3. `lists`: with a listener, a UDP socket, a file, a directory listing, a signal subscription, named memory and
   the reactor all open, the program starts `python3` twice, with `Process.spawn` and with a handover of two
   places: the first child has no descriptor above 2, the second exactly 3 and 4 (POSIX);
4. `plain`: a program started with nothing handed over has an empty `Inherited`.

The interpreter starts no children: `spawnWith` is `Unsupported` there and `Process.inherited()` is empty.

usage: python3 tools/handover_check.py --compiler out/ceesafec
"""

import argparse
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time

PROGRAM = r'''async fn nap(ms: u64) -> Result<void, AsyncError> => await Timer.sleep(ms);

fn pause(ms: u64) {
    let _ = Async.run(nap(ms));
}

fn started(me: StringView, how: StringView, which: StringView, handover: borrow Handover) -> ChildProcess {
    var arguments = Vector<StringView>.create();
    arguments.push(how)!;
    arguments.push(which)!;
    return Process.spawnWith(me, arguments, ChildOptions.inherit(), handover)!;
}

/// Accepts from the listener it was handed until it is ended, answering each connection and counting it.
async fn serving(listener: borrow AsyncTcpListener, memory: borrow SharedMemory, mine: usize) -> Result<void, AsyncError> {
    loop {
        let stream = await listener.accept()?;
        memory.word(0).fetchAdd(1);
        memory.word(mine).fetchAdd(1);
        await stream.write("ok\n".bytes())?;
    }
    return Ok();
}

fn notFound<T>(r: Result<T, IoError>) -> bool => match (r) {
    Err(IoError.NotFound) => true,
    _ => false,
};

fn worker(number: StringView) -> i32 {
    var given = Process.inherited();
    let first = number == "0";
    var right = given.count() == 2 && (given.kind(0) is Some(InheritedKind.Listener))
        && (given.kind(1) is Some(InheritedKind.Memory)) && given.kind(2).isNull();
    if (first) {
        // A place of the other kind, and one past the end.
        right = right && notFound(given.memory(0)) && notFound(given.listener(1)) && notFound(given.listener(7))
            && notFound(given.datagrams(0));
    }
    let listener = match (given.listener(0)) {
        Ok(l) => l,
        Err(_) => { return 3; },
    };
    let memory = match (given.memory(1)) {
        Ok(m) => m,
        Err(_) => { return 4; },
    };
    if (first) {
        // Taken: not there again, and of no kind.
        right = right && notFound(given.listener(0)) && notFound(given.memory(1)) && given.kind(0).isNull();
        // A program this one starts is handed nothing.
        let me = Process.executable()!;
        var arguments = Vector<StringView>.create();
        arguments.push("plain")!;
        arguments.push("grandchild")!;
        var grandchild = Process.spawn(me.view(), arguments, ChildOptions.inherit())!;
        right = right && (grandchild.wait()! is ChildExit.Status(0));
        memory.word(8).store(if (right) 1u64 else 2u64);
    }
    let mine: usize = if (number == "0") 1 else if (number == "1") 2 else 3;
    memory.word(4).fetchAdd(1); // ready
    let _ = Async.run(serving(listener, memory, mine));
    return 0;
}

fn parent() {
    let me = Process.executable()!;
    let memory = SharedMemory.anonymous(4096)!;
    var port: u16 = 0;
    var handover = Handover.create();
    var workers = Vector<ChildProcess>.create();
    {
        let listener = AsyncTcpListener.bind("127.0.0.1", 0)!;
        port = listener.port()!;
        Console.writeLine(if (handover.listener(listener)! == 0 && handover.memory(memory)! == 1 && handover.length() == 2) "listed" else "places are off");
        workers.push(started(me.view(), "worker", "0", handover))!;
        workers.push(started(me.view(), "worker", "1", handover))!;
        // The parent's own listener goes: the workers have the socket.
    }
    // A worker started later is handed the same things.
    workers.push(started(me.view(), "worker", "2", handover))!;
    var waited = 0;
    while (memory.word(4).load() < 3 && waited < 200) {
        pause(25);
        waited += 1;
    }
    Console.write("port ");
    Console.writeLine(port as u64);
    Console.flush();
    // The script connects 300 times; then it is told.
    waited = 0;
    while (memory.word(0).load() < 300 && waited < 1200) {
        pause(25);
        waited += 1;
    }
    Console.writeLine(memory.word(0).load());
    let each = memory.word(1).load() + memory.word(2).load() + memory.word(3).load();
    Console.writeLine(if (each == memory.word(0).load()) "the workers' counts sum to it" else "the counts do not sum");
    Console.writeLine(if (memory.word(8).load() == 1) "places as they should be" else "places are off");
    for (i in 0..workers.length) {
        workers[i].kill()!;
        let _ = workers[i].wait()!;
    }
    // Nothing of the socket is left: the handover's hold goes, and the port can be bound again.
    {
        let dropped = handover;
    }
    match (AsyncTcpListener.bind("127.0.0.1", port)) {
        Ok(_) => { Console.writeLine("the port is free again"); }
        Err(_) => { Console.writeLine("the port is still held"); }
    }
    Console.writeLine("done");
}

async fn receiving(socket: borrow AsyncUdpSocket, memory: borrow SharedMemory) -> Result<void, AsyncError> {
    var buffer = [0u8; 64];
    let got = await socket.recvFrom(mut buffer[..])?;
    memory.word(0).fetchAdd(1);
    return Ok();
}

fn hearer() -> i32 {
    var given = Process.inherited();
    let socket = match (given.datagrams(0)) {
        Ok(s) => s,
        Err(_) => { return 3; },
    };
    let memory = match (given.memory(1)) {
        Ok(m) => m,
        Err(_) => { return 4; },
    };
    memory.word(4).fetchAdd(1);
    return match (Async.run(receiving(socket, memory))) {
        Ok() => 0,
        Err(_) => 5,
    };
}

fn datagrams() {
    let me = Process.executable()!;
    let memory = SharedMemory.anonymous(64)!;
    let socket = AsyncUdpSocket.bind(SocketAddress.V4([127u8, 0, 0, 1], 0), UdpOptions.defaults())!;
    var handover = Handover.create();
    let _ = handover.datagrams(socket)!;
    let _ = handover.memory(memory)!;
    var one = started(me.view(), "hearer", "0", handover);
    var two = started(me.view(), "hearer", "1", handover);
    var waited = 0;
    while (memory.word(4).load() < 2 && waited < 200) {
        pause(25);
        waited += 1;
    }
    Console.write("port ");
    Console.writeLine(socket.localAddress()!.port() as u64);
    Console.flush();
    let first = one.wait()!;
    let second = two.wait()!;
    Console.writeLine(if ((first is ChildExit.Status(0)) && (second is ChildExit.Status(0))) "both heard" else "one did not");
    Console.writeLine(memory.word(0).load());
}

async fn lists(path: StringView) -> Result<void, AsyncError> {
    let listener = match (AsyncTcpListener.bind("127.0.0.1", 0)) {
        Ok(l) => l,
        Err(e) => { return Err(AsyncError.Io(e)); },
    };
    let socket = match (AsyncUdpSocket.bind(SocketAddress.V4([127u8, 0, 0, 1], 0), UdpOptions.defaults())) {
        Ok(s) => s,
        Err(e) => { return Err(AsyncError.Io(e)); },
    };
    let file = await AsyncFiles.open(path)?;
    // A connection made and accepted, so that both ends are open here.
    let port = match (listener.port()) {
        Ok(p) => p,
        Err(e) => { return Err(AsyncError.Io(e)); },
    };
    let out = await AsyncTcpStream.connect("127.0.0.1", port)?;
    let accepted = await listener.accept()?;
    await Timer.sleep(10)?;
    var arguments = Vector<StringView>.create();
    arguments.push("-c")!;
    arguments.push("import os\nopen_ = []\nfor fd in range(3, 256):\n    try:\n        os.fstat(fd)\n        open_.append(str(fd))\n    except OSError:\n        pass\nprint('open: ' + ' '.join(open_), flush=True)")!;
    for (round in 0..2) {
        var handover = Handover.create();
        if (round == 1) {
            match (handover.listener(listener)) {
                Ok(_) => {}
                Err(e) => { return Err(AsyncError.Io(e)); }
            }
            match (handover.datagrams(socket)) {
                Ok(_) => {}
                Err(e) => { return Err(AsyncError.Io(e)); }
            }
        }
        var child = match (Process.spawnWith("python3", arguments, ChildOptions.inherit(), handover)) {
            Ok(c) => c,
            Err(e) => { return Err(AsyncError.Io(e)); },
        };
        loop {
            match (child.poll()) {
                Ok(Some(_)) => { break; }
                Ok(null) => { await Timer.sleep(20)?; }
                Err(e) => { return Err(AsyncError.Io(e)); }
            }
        }
    }
    return Ok();
}

fn main() {
    let how = Process.argument(1) ?? "";
    let which = Process.argument(2) ?? "";
    if (how == "worker") {
        Process.exit(worker(which));
    }
    if (how == "hearer") {
        Process.exit(hearer());
    }
    if (how == "parent") {
        parent();
        return;
    }
    if (how == "datagrams") {
        datagrams();
        return;
    }
    if (how == "plain") {
        var given = Process.inherited();
        let empty = given.count() == 0 && given.kind(0).isNull() && notFound(given.listener(0));
        if (which == "grandchild") {
            Process.exit(if (empty) 0 else 9);
        }
        Console.writeLine(if (empty) "nothing handed" else "something handed");
        return;
    }
    if (how == "lists") {
        let signals = ProcessSignals.subscribe()!;
        let memory = SharedMemory.create(which, 64)!;
        let names = Directory.list(".")!;
        let me = Process.executable()!;
        match (Async.run(lists(me.view()))) {
            Ok() => { Console.writeLine("listed"); }
            Err(_) => { Console.writeLine("failed"); }
        }
    }
}
'''

INTERPRETED = r'''fn main() {
    var given = Process.inherited();
    Console.writeLine(given.count());
    Console.writeLine(if (given.kind(0).isNull() && given.listener(0).isErr()) "nothing to take" else "something?");
    var handover = Handover.create();
    var none = Vector<StringView>.create();
    match (Process.spawnWith("true", none, ChildOptions.quiet(), handover)) {
        Ok(_) => { Console.writeLine("started"); }
        Err(e) => { Console.writeLine(if (e is IoError.Unsupported) "unsupported" else "another error"); }
    }
    match (SharedMemory.anonymous(64)) {
        Ok(_) => { Console.writeLine("made"); }
        Err(e) => { Console.writeLine(if (e is IoError.Unsupported) "unsupported" else "another error"); }
    }
}
'''

DEADLINE = 120.0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    windows = os.name == "nt"
    problems = []
    cases = 0

    def expect(what, got, wanted):
        nonlocal cases
        cases += 1
        if got != wanted:
            problems.append(f"{what}: {got!r}, expected {wanted!r}")

    with tempfile.TemporaryDirectory(prefix="ceesafe-handover-") as work:
        source = os.path.join(work, "handover.cee")
        with open(source, "w", encoding="utf-8") as f:
            f.write(PROGRAM)
        exe = os.path.join(work, "handover.exe" if windows else "handover")
        built = subprocess.run([compiler, "build", source, "-o", exe], capture_output=True, text=True)
        if built.returncode != 0:
            print(f"FAIL build: {built.stderr.strip()[:1200]}")
            print("handover_check: 0 cases: 1 problems")
            return 1

        # 1. Three workers accept from one listener and count in one memory.
        parent = subprocess.Popen([exe, "parent"], stdout=subprocess.PIPE, text=True)
        watchdog = threading.Timer(DEADLINE, parent.kill)
        watchdog.start()
        lines = []
        port = 0
        while True:
            line = parent.stdout.readline()
            if not line:
                break
            lines.append(line.strip())
            if line.startswith("port "):
                port = int(line.split()[1])
                break
        answered = 0
        if port:
            for _ in range(300):
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=10) as c:
                        if c.recv(16) == b"ok\n":
                            answered += 1
                except OSError:
                    pass
        lines += [line.strip() for line in parent.stdout.read().splitlines()]
        parent.wait()
        watchdog.cancel()
        expect("300 connections answered", answered, 300)
        expect("parent", [line for line in lines if not line.startswith("port ")],
               ["listed", "300", "the workers' counts sum to it", "places as they should be", "the port is free again",
                "done"])

        # 2. A UDP socket handed to two children.
        both = subprocess.Popen([exe, "datagrams"], stdout=subprocess.PIPE, text=True)
        watchdog = threading.Timer(DEADLINE, both.kill)
        watchdog.start()
        line = both.stdout.readline()
        if line.startswith("port "):
            sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            for _ in range(2):
                sender.sendto(b"hello", ("127.0.0.1", int(line.split()[1])))
                time.sleep(0.2)
            sender.close()
        rest = [text.strip() for text in both.stdout.read().splitlines()]
        both.wait()
        watchdog.cancel()
        expect("datagrams", rest, ["both heard", "2"])

        # 3. What a child has open.
        if not windows:
            r = subprocess.run([exe, "lists", f"cee-0173-{os.getpid()}"], capture_output=True, text=True, timeout=DEADLINE)
            expect("descriptors of a spawned child, then of one handed two", r.stdout.splitlines(),
                   ["open: ", "open: 3 4", "listed"])

        # 4. Nothing handed.
        r = subprocess.run([exe, "plain"], capture_output=True, text=True, timeout=DEADLINE)
        expect("plain", r.stdout.splitlines(), ["nothing handed"])

        path = os.path.join(work, "interpreted.cee")
        with open(path, "w", encoding="utf-8") as f:
            f.write(INTERPRETED)
        r = subprocess.run([compiler, "run", "--interp", path], capture_output=True, text=True, timeout=DEADLINE)
        expect("interpreted", r.stdout.splitlines(), ["0", "nothing to take", "unsupported", "unsupported"])

    for problem in problems:
        print(f"FAIL {problem}")
    print(f"handover_check: {cases} cases: {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
