---
title: One connection at a time
summary: The first stage of the web server - TcpListener and TcpStream, a loop that accepts one connection after another, reading a request up to its empty line, answering with a fixed page, and a test that runs the exchange on a port the system chooses.
assumes: [ch21-00-web-server-project, ch09-02-result, ch11-01-writing-tests, ch08-02-strings]
keywords: [TcpListener, TcpStream, bind, accept, read, write, shutdownWrite, port 0, loopback, "Console.flush", Content-Length, HTTP response]
diagnostics: [N0402, N0248]
specification: ["type-system.md §26.4", "type-system.md §26.3"]
---

# One connection at a time

**This section teaches** the library's TCP sockets, and builds the first stage of the server: it accepts a
connection, reads the request, answers with one fixed page, and accepts the next. It assumes the chapter's
[opening page](./ch21-00-web-server-project), `Result` and `?`
([Recoverable errors with `Result`](./ch09-02-result)), tests ([Writing tests](./ch11-01-writing-tests)) and
`String` ([Strings](./ch08-02-strings)).

## A listener and a stream

Two types do the work. A `TcpListener` waits for connections on a port. A `TcpStream` is one connection, and both
ends of a connection hold one. The program below is both ends at once, so that it can run by itself.

<span class="listing-caption"><strong>Listing 21-1</strong>, <code>main.cee</code>: A listener, a client and one exchange, in one program on one thread.</span>

```ceesafe
fn main() {
    let listener = TcpListener.bind("127.0.0.1", 0)!;
    let port = listener.port()!;
    var client = TcpStream.connect("127.0.0.1", port)!;
    client.write("is anybody there?".bytes())!;
    client.shutdownWrite()!;

    var server = listener.accept()!;
    var buffer = [0u8; 64];
    let n = server.read(mut buffer[..])!;
    Console.write("the server read: ");
    Console.writeLine(StringView.fromUtf8(buffer[0..n]) ?? "?");
    server.write("yes".bytes())!;
    server.shutdownWrite()!;

    let m = client.read(mut buffer[..])!;
    Console.write("the client read: ");
    Console.writeLine(StringView.fromUtf8(buffer[0..m]) ?? "?");
    Console.write("then: ");
    Console.writeLine(client.read(mut buffer[..])!);
}
```

```text
the server read: is anybody there?
the client read: yes
then: 0
```

- `TcpListener.bind(address, port)` starts listening. Port `0` asks the system for any free port, and
  `listener.port()` says which one it chose. A test uses port 0 so that it never meets a port that is taken.
- `TcpStream.connect(address, port)` is the client's side. The system completes the connection and keeps it in a
  queue, so the client can connect and write before the server has called `accept`.
- `listener.accept()` takes the next connection from the queue, and waits if there is none. Its result is the
  server's end, another `TcpStream`.
- `read(mut buffer[..])` waits for bytes and returns how many it put in the buffer. `write(bytes)` sends all the
  bytes it is given.
- `shutdownWrite()` says "I will send no more". The other side's `read` then returns `0`, as the last line shows.

Every one of these calls can fail, and returns a `Result` with an `IoError`. The listing stops on any error with
`!`. The server below passes errors on with `?`.

::: tip Rule
`read` returns as soon as it has some bytes, not when it has all that the other side sent. A result of `0` means
the other side has finished sending. Read in a loop until you have what you need.

*Specification: `type-system.md` §26.4.*
:::

::: tip Rule
A `TcpListener` and a `TcpStream` own their sockets. Destroying one closes it: there is no `close` to call, and
none to forget.

*Specification: `type-system.md` §26.4.*
:::

## Reading the request

In `src/main.cee` of the new package, start with the error type and the function that reads a request.

<span class="listing-caption"><strong>Listing 21-2</strong>, <code>src/main.cee</code>: The error type, and reading up to the empty line that ends a request's head.</span>

<!-- ceesafe: skip -->
```ceesafe
/// What can go wrong while a connection is answered.
enum ServeError {
    @from Io(IoError),
    @from Memory(AllocError),
}

/// Reads from `stream` until the empty line that ends a request's head, or the end of the stream.
fn readHead(stream: borrow mut TcpStream) -> Result<Vector<u8>, ServeError> {
    var head = Vector<u8>.create();
    var buffer = [0u8; 512];
    loop {
        let n = stream.read(mut buffer[..])?;
        if (n == 0) {
            return Ok(head);
        }
        head.extend(buffer[0..n])?;
        if (head.view().contains("\r\n\r\n".bytes())) {
            return Ok(head);
        }
    }
}
```

A request's head ends with an empty line, so the bytes `\r\n\r\n` mark its end. A browser keeps the connection open
after its request, so the server cannot wait for `read` to return `0`: it reads until it has seen the mark.
`readHead` collects the pieces in a vector, because one `read` may bring half a request, or all of it.

The two `@from` variants let `?` convert the two errors the function meets: an `IoError` from `read`, and an
`AllocError` from `extend`, which may need memory.

## Answering

<span class="listing-caption"><strong>Listing 21-3</strong>, <code>src/main.cee</code>: The page, and the function that answers one connection with it.</span>

<!-- ceesafe: skip -->
```ceesafe
/// Answers one connection: reads the request, and sends the page whatever the request was.
fn handle(stream: borrow mut TcpStream) -> Result<void, ServeError> {
    let _ = readHead(mut stream)?;
    var response = String.create();
    response.append("HTTP/1.1 200 OK\r\n")?;
    response.append("Content-Type: text/html; charset=utf-8\r\n")?;
    response.append("Content-Length: ")?;
    response.appendNumber(PAGE.length as u64)?;
    response.append("\r\nConnection: close\r\n\r\n")?;
    response.append(PAGE)?;
    stream.write(response.view().bytes())?;
    stream.shutdownWrite()?;
    return Ok();
}
```

`PAGE` is a constant at the top of the file:

```text
const PAGE: StringView = "<!doctype html>\n<title>Harbour</title>\n<h1>The harbour is open.</h1>\n";
```

The response is a status line, three headers, an empty line and the body:

- `HTTP/1.1 200 OK` says that the request succeeded.
- `Content-Type` says what the body is, so that a browser shows it as a page.
- `Content-Length` is the body's length in bytes. `PAGE.length` is that number: the length of a view is in bytes.
- `Connection: close` says that the server will close the connection after this response. HTTP allows several
  requests on one connection. This server answers one, which keeps every stage simple.

This stage reads the request and ignores it: `let _ = …` discards the bytes. It still has to read them. A server
that answers and closes while the client is sending may cut the client off before it has read the answer.

## The loop

<span class="listing-caption"><strong>Listing 21-4</strong>, <code>src/main.cee</code>: <code>main</code> accepts connections for ever, one after another.</span>

<!-- ceesafe: skip -->
```ceesafe
fn main() {
    let listener = TcpListener.bind("127.0.0.1", 7878)!;
    Console.writeLine("listening on http://127.0.0.1:7878");
    Console.flush();
    loop {
        var stream = listener.accept()!;
        match (handle(mut stream)) {
            Ok() => {}
            Err(_) => { Console.writeErrorLine("a connection failed"); }
        }
    }
}
```

- The port is fixed at 7878, so that you know where to point a browser.
- `stream` is declared in the loop's body, so it is destroyed at the end of each turn. That closes the connection.
- A failed connection is reported and forgotten. One client that goes away in the middle of its request must not
  stop the server for the others.
- `Console.flush()` is needed because standard output is buffered: the line would otherwise wait in memory while
  the program waits in `accept`, and a server that is stopped from outside never writes it.

::: tip Rule
`Console.write` and `Console.writeLine` collect their output in memory and write it later: before the program
reads input or writes to standard error, and when it ends. Call `Console.flush()` before a long wait. Standard
error is not buffered.

*Specification: `type-system.md` §26.3.*
:::

Build it, run it, and ask for a page from a second terminal:

```sh
ceesafec build
./out/harbour
```

```sh
curl -i http://127.0.0.1:7878/
```

```text
HTTP/1.1 200 OK
Content-Type: text/html; charset=utf-8
Content-Length: 69
Connection: close

<!doctype html>
<title>Harbour</title>
<h1>The harbour is open.</h1>
```

Every path gives the same page: `/about` and `/nowhere` are answered as `/` is. Stop the server with Ctrl+C.

## A test that is a client

`main` never returns, so a test cannot call it. It can call `handle`, in the way Listing 21-1 does: connect, write
a request, accept, answer, read.

<span class="listing-caption"><strong>Listing 21-5</strong>, <code>src/main.cee</code>: A test that sends a request to <code>handle</code> through a real connection.</span>

<!-- ceesafe: skip -->
```ceesafe
@test
fn answersARequest() -> Result<void, TestFailure> {
    let listener = TcpListener.bind("127.0.0.1", 0)!;
    let port = listener.port()!;
    var client = TcpStream.connect("127.0.0.1", port)!;
    client.write("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n".bytes())!;
    var server = listener.accept()!;
    match (handle(mut server)) {
        Ok() => {}
        Err(_) => { return Check.fail("the connection failed"); }
    }
    var reply = String.create();
    var buffer = [0u8; 256];
    loop {
        let n = client.read(mut buffer[..])!;
        if (n == 0) {
            break;
        }
        reply.append(StringView.fromUtf8(buffer[0..n]) ?? "?")!;
    }
    Check.isTrue(reply.view().startsWith("HTTP/1.1 200 OK\r\n"), "the status line is 200 OK")?;
    Check.isTrue(reply.view().endsWith(PAGE), "the body is the page")?;
    return Ok();
}
```

```sh
ceesafec test
```

```text
test answersARequest ... ok

ceesafec test: 1 passed, 0 failed
```

The client's loop reads until `0`. It ends because `handle` calls `shutdownWrite()` after the response. The test
runs on one thread, in an order that cannot block: the request is written before `handle` reads it, and the
response is written before the client reads it. Both are small enough to wait in the system's buffers.

## Mistakes

::: warning Mistake: not looking at the result of `write`
A write to a connection fails when the client has gone. A call statement that drops the `Result` hides that.
:::

<span class="listing-caption"><strong>Listing 21-6</strong>, <code>main.cee</code>: The result of <code>write</code> is not used. The compiler warns.</span>

<!-- ceesafe: warns N0402 -->
```ceesafe
fn main() {
    let listener = TcpListener.bind("127.0.0.1", 0)!;
    var client = TcpStream.connect("127.0.0.1", listener.port()!)!;
    var server = listener.accept()!;
    server.write("HTTP/1.1 200 OK\r\n\r\n".bytes());
    server.shutdownWrite()!;
    var buffer = [0u8; 64];
    Console.writeLine(client.read(mut buffer[..])!);
}
```

```text
main.cee:5:5: warning[N0402]: the 'Result' of this call is not used, so an error it reports is lost: handle it with '?', '!' or 'match', or write 'let _ = ...;' to discard it
  |
5 |     server.write("HTTP/1.1 200 OK\r\n\r\n".bytes());
  |     ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
```

In a function that returns a `Result`, write `server.write(…)?;`.

::: warning Mistake: a stream declared with `let`
Reading and writing change the stream, so `read`, `write` and a `borrow mut TcpStream` parameter need a variable
declared with `var`.
:::

<span class="listing-caption"><strong>Listing 21-7</strong>, <code>main.cee</code>: The accepted stream is a <code>let</code>. This does not compile.</span>

<!-- ceesafe: fail N0248 -->
```ceesafe
fn main() {
    let listener = TcpListener.bind("127.0.0.1", 0)!;
    var client = TcpStream.connect("127.0.0.1", listener.port()!)!;
    client.write("GET / HTTP/1.1\r\n\r\n".bytes())!;
    let server = listener.accept()!;
    var buffer = [0u8; 64];
    Console.writeLine(server.read(mut buffer[..])!);
}
```

```text
main.cee:7:23: error[N0248]: cannot borrow 'server' as mutable: 'server' was declared with 'let'; use 'var' for a mutable variable
  |
7 |     Console.writeLine(server.read(mut buffer[..])!);
  |                       ^^^^^^
```

The listener is a `let`: `accept` and `port` do not change it.

::: warning Mistake: taking one `read` for the whole request
`stream.read(mut buffer[..])` once, and then parsing what came, works on your own machine and fails in use: a
request may arrive in several pieces. No diagnostic tells you. Read until the empty line, as `readHead` does.
:::

::: details Coming from Rust
`TcpListener.bind("127.0.0.1", 7878)` takes the address and the port apart, not one `"127.0.0.1:7878"`. There is
no `incoming()` iterator: call `accept()` in a loop. `write` is Rust's `write_all`: it sends everything or fails.
A stream is closed when it is destroyed, as in Rust.
:::

::: details Coming from Go, Python or JavaScript
There is no `defer conn.Close()` and no `with` block: the stream is closed where its variable's life ends, on every
path, including the `?` that returns early. Errors are values in a `Result`, not exceptions, and the compiler
warns when one is dropped.
:::

## Summary

- `TcpListener.bind(address, port)` listens; port `0` lets the system choose, and `port()` gives the choice.
  `accept()` waits for a connection and returns its `TcpStream`.
- `read` gives some bytes, or `0` at the end. `write` sends all its bytes. `shutdownWrite()` ends this side's
  sending.
- Destroying a listener or a stream closes it.
- A response is a status line, headers with `Content-Length`, an empty line and the body.
- Standard output is buffered: `Console.flush()` before a long wait.
- An unused `Result` is warning `N0402`. A stream that is read or written is a `var` (`N0248`).

The server answers every request with the same page, and one client at a time. Next:
[requests and responses](./ch21-02-requests-and-responses), where it reads what it is asked.
