---
title: Requests and responses
summary: The second stage of the web server - a second file of pure code that parses a request into views of its text, chooses a response by method and path with pages embedded by embedText, and renders it, each part with tests that need no socket.
assumes: [ch21-01-one-connection, ch10-03-regions, ch08-02-strings, ch06-03-optionals, ch12-04-test-first]
keywords: [HTTP, request line, headers, parse, split, splitOnce, region, StringView, embedText, route, 404, 400, 405, Content-Length, pure code, tests]
diagnostics: [N0244, N0362]
specification: ["type-system.md §10.1"]
---

# Requests and responses

**This section teaches** how to turn the bytes of a request into a value, and a value into the bytes of a
response, in code that touches no socket and so is easy to test. It adds a second file to the server of
[section 21.1](./ch21-01-one-connection). It assumes regions ([Regions](./ch10-03-regions)), the text functions
of [Strings](./ch08-02-strings), [optionals](./ch06-03-optionals), and the method of
[Adding the search, test first](./ch12-04-test-first).

The plan is to keep two kinds of code apart:

- **Pure code** takes text and returns values: parse the request, choose the response, render it. It goes in
  `src/http.cee`. It cannot fail because of the network, and a test calls it with a string literal.
- **The edges** read from and write to the connection. They stay in `src/main.cee`, and stay small.

The later sections replace the edges twice, with threads and then with tasks. `src/http.cee` does not change
again.

## A request as a value

<span class="listing-caption"><strong>Listing 21-8</strong>, <code>src/http.cee</code>: The types of a parsed request.</span>

<!-- ceesafe: skip -->
```ceesafe
/// What a request asks the server to do. This server knows one method.
enum Method {
    Get,
    Other,
}

/// One header of a request: `Host: localhost` has the name `Host` and the value `localhost`.
struct Header<region r> {
    name: StringView<r>;
    value: StringView<r>;
}

/// A request's head, as views into the text it was parsed from.
struct Request<region r> {
    method: Method;
    path: StringView<r>;
    headers: Vector<Header<r>>;
}
```

A `Request` copies nothing out of the text it comes from: its path and its headers are views into that text. A
struct that holds views says so with a region parameter, `<region r>`, and the compiler then checks that the text
outlives the request.

Write the tests before the parser. They say what a request is:

<span class="listing-caption"><strong>Listing 21-9</strong>, <code>src/http.cee</code>: Two tests for the parser.</span>

<!-- ceesafe: skip -->
```ceesafe
@test
fn parsesTheRequestLine() -> Result<void, TestFailure> {
    let request = parseRequest("GET /about HTTP/1.1\r\nHost: localhost\r\n\r\n")!;
    Check.equalText("/about", request.path)?;
    Check.isTrue(request.method is Method.Get, "the method is GET")?;
    return Ok();
}

@test
fn parsesHeaders() -> Result<void, TestFailure> {
    let request = parseRequest("GET / HTTP/1.1\r\nHost: localhost:7878\r\nAccept:  text/html \r\n\r\n")!;
    Check.equalU64(2, request.headers.length as u64)?;
    Check.equalText("localhost:7878", request.header("host") ?? "")?;
    Check.equalText("text/html", request.header("ACCEPT") ?? "")?;
    Check.isTrue(request.header("Cookie").isNull(), "there is no Cookie header")?;
    return Ok();
}
```

The second test fixes two details of HTTP: spaces around a header's value do not count, and a header's name
matches in any case.

## The parser

<span class="listing-caption"><strong>Listing 21-10</strong>, <code>src/http.cee</code>: The error type and <code>parseRequest</code>.</span>

<!-- ceesafe: skip -->
```ceesafe
/// Why a text is not a request's head.
enum ParseError {
    BadRequestLine,
    BadVersion,
    BadHeader,
    Incomplete,
    @from Memory(AllocError),
}

/// Parses a request's head: the request line, then one header on each line, up to the empty line that ends it.
fn parseRequest<region r>(head: StringView<r>) -> Result<Request<r>, ParseError> {
    if (!head.contains("\r\n\r\n")) {
        return Err(ParseError.Incomplete);
    }
    var lines = head.split("\r\n");
    let first = lines.next() ?? "";
    var words = first.split(" ");
    let method = words.next() ?? "";
    let path = words.next() ?? "";
    let version = words.next() ?? "";
    if (method.length == 0 || !path.startsWith("/") || words.next().isSome()) {
        return Err(ParseError.BadRequestLine);
    }
    if (version != "HTTP/1.1" && version != "HTTP/1.0") {
        return Err(ParseError.BadVersion);
    }
    var headers = Vector<Header<r>>.create();
    for (line in lines) {
        if (line.length == 0) {
            break;
        }
        match (line.splitOnce(":")) {
            Some(pair) => {
                if (pair.left.length == 0 || pair.left.contains(" ")) {
                    return Err(ParseError.BadHeader);
                }
                headers.push(Header { name: pair.left, value: pair.right.trim() })?;
            }
            null => { return Err(ParseError.BadHeader); }
        }
    }
    let known = if (method == "GET") Method.Get else Method.Other;
    return Ok(Request { method: known, path: path, headers: move headers });
}
```

- A head without its empty line is not whole, and is refused before anything else is looked at.
- `head.split("\r\n")` gives the lines one at a time. The value it returns has a `next()` method, which gives the
  next piece, or `null` when there are no more. The parser takes the first line by hand with `lines.next()`, and
  the `for` loop then goes on from the second.
- `?? ""` makes a missing word an empty one, so the three checks that follow cover a request line that is too
  short. `words.next().isSome()` after the third word catches one that is too long.
- `line.splitOnce(":")` cuts a header at its first colon. A value may contain colons, as `localhost:7878` does.
- The signature says `StringView<r>` in, `Request<r>` out: the request borrows from the text, with the same region
  parameter that `search` has in chapter 12.

The method that looks a header up goes in an `impl` block for the struct:

<span class="listing-caption"><strong>Listing 21-11</strong>, <code>src/http.cee</code>: Finding a header by its name, in any case.</span>

<!-- ceesafe: skip -->
```ceesafe
impl<region r> Request<r> {
    /// The value of the header `name`, in whatever case the client wrote the name.
    fn header(borrow this, name: StringView) -> StringView<r>? {
        for (i in 0..this.headers.length) {
            if (this.headers[i].name.equalsIgnoreAsciiCase(name)) {
                return Some(this.headers[i].value);
            }
        }
        return null;
    }
}
```

The result is `StringView<r>?`: a view into the request's text, not into the `Request` and not into `name`. So
the value may be used after the `Request` is gone, as long as the text is there.

A parser is also what it refuses. A third test, `refusesWhatIsNotARequest` in the package, gives it an empty text, a
request line with no path, a path without `/`, a fourth word, the version `HTTP/3`, a header without a colon and
a head that stops before its empty line, and checks the error for each.

## Pages in the program

The server now has three pages. Put them in a directory `pages` beside `ceesafe.toml`: `index.html`, `about.html`
and `404.html`. The first is the page the opening of this chapter shows.

<span class="listing-caption"><strong>Listing 21-12</strong>, <code>src/http.cee</code>: The pages, a response, and the function that chooses one.</span>

<!-- ceesafe: skip -->
```ceesafe
/// The pages, read from the directory `pages` when the package is compiled.
const INDEX: StringView = embedText("../pages/index.html");
const ABOUT: StringView = embedText("../pages/about.html");
const NOT_FOUND: StringView = embedText("../pages/404.html");

const HTML: StringView = "text/html; charset=utf-8";
const PLAIN: StringView = "text/plain; charset=utf-8";

/// An answer: a status, and a body with its type.
struct Response<region r> {
    status: u16;
    reason: StringView<r>;
    contentType: StringView<r>;
    body: StringView<r>;
}

/// The answer to a request, chosen by its method and its path.
fn route(request: borrow Request) -> Response<static> {
    match (request.method) {
        Method.Get => {}
        Method.Other => {
            return Response {
                status: 405,
                reason: "Method Not Allowed",
                contentType: PLAIN,
                body: "Only GET is served here.\n",
            };
        }
    }
    if (request.path == "/") {
        return Response { status: 200, reason: "OK", contentType: HTML, body: INDEX };
    }
    if (request.path == "/about") {
        return Response { status: 200, reason: "OK", contentType: HTML, body: ABOUT };
    }
    return Response { status: 404, reason: "Not Found", contentType: HTML, body: NOT_FOUND };
}
```

`embedText(path)` is a built-in function. The compiler reads the file when it compiles the package, and the
file's text becomes part of the program, as a string literal is. The executable needs no `pages` directory beside
it when it runs, and a page that is missing is a compile error, not a failure at the first request.

::: tip Rule
`embedText(path)` is the text of a file as a `StringView`, and `embed(path)` its bytes as a `Slice<u8>`, read at
compile time. The path is a string literal, relative to the directory of the source file that names it, and must
lead to a file inside the package.

*Specification: `type-system.md` §10.1.*
:::

`route` returns a `Response<static>`: every view in it is a literal or an embedded file, which last as long as
the program. So a response borrows nothing from the request, and the request's text can be released before the
response is sent.

The paths are compared whole, with `==`. The server never builds a file name from what the client sent, so a
request for `/../secret` cannot reach a file: it is one more path that is not on the list.

<span class="listing-caption"><strong>Listing 21-13</strong>, <code>src/http.cee</code>: From the text of a request to a response, and from a response to the text to send.</span>

<!-- ceesafe: skip -->
```ceesafe
/// The answer to a text that is not a request.
fn badRequest() -> Response<static> =>
    Response { status: 400, reason: "Bad Request", contentType: PLAIN, body: "That is not an HTTP request.\n" };

/// The answer to the text of a request's head: parsed and routed, or 400.
fn answer(head: StringView) -> Response<static> => match (parseRequest(head)) {
    Ok(request) => route(borrow request),
    Err(_) => badRequest(),
};

/// The response as the text to send: a status line, three headers, an empty line and the body.
fn render(response: borrow Response) -> Result<String, AllocError> {
    var out = String.create();
    out.append("HTTP/1.1 ")?;
    out.appendNumber(response.status as u64)?;
    out.append(" ")?;
    out.append(response.reason)?;
    out.append("\r\nContent-Type: ")?;
    out.append(response.contentType)?;
    out.append("\r\nContent-Length: ")?;
    out.appendNumber(response.body.length as u64)?;
    out.append("\r\nConnection: close\r\n\r\n")?;
    out.append(response.body)?;
    return Ok(out);
}
```

A request that cannot be parsed is not an error of the server. It has an answer, status 400, and the connection
is handled as any other. `answer` is the whole of the pure code in one call, and the package's tests `routesByPath`
and `rendersAResponse` check it with string literals:

```text
Check.equalU64(404, answer("GET /nowhere HTTP/1.1\r\n\r\n").status as u64)?;
```

## The edges

`src/main.cee` keeps `readHead`, `handle`, `main` and the test. `handle` now uses the pure code:

<span class="listing-caption"><strong>Listing 21-14</strong>, <code>src/main.cee</code>: <code>handle</code> reads, asks <code>answer</code> for the response, renders it and writes.</span>

<!-- ceesafe: skip -->
```ceesafe
/// Answers one connection: reads the request, chooses the response and sends it.
fn handle(stream: borrow mut TcpStream) -> Result<void, ServeError> {
    let head = readHead(mut stream)?;
    let response = match (StringView.fromUtf8(head.view())) {
        Some(text) => answer(text),
        null => badRequest(),
    };
    let text = render(borrow response)?;
    stream.write(text.view().bytes())?;
    stream.shutdownWrite()?;
    return Ok();
}
```

`StringView.fromUtf8` returns `null` for bytes that are not text, and those get the 400 too.

`readHead` gets one change. A client could send bytes for ever without an empty line, and the vector would grow
until memory ran out. The loop now stops at a limit, `const HEAD_LIMIT: usize = 8192;`: its `loop` becomes
`while (head.length < HEAD_LIMIT)`. A head that was cut short has no empty line, so the parser returns
`Incomplete` and the client is answered with 400.

The test of section 21.1 becomes a function `fetch` that makes one exchange, and a test that calls it twice, for
`/` and for `/nowhere`. Both are in the package. Run everything:

```sh
ceesafec test
```

```text
test parsesHeaders ... ok
test parsesTheRequestLine ... ok
test refusesWhatIsNotARequest ... ok
test rendersAResponse ... ok
test routesByPath ... ok
test servesThePages ... ok

ceesafec test: 6 passed, 0 failed
```

Five of the six tests open no socket. And with the server built and running:

```sh
curl -i http://127.0.0.1:7878/nowhere
```

```text
HTTP/1.1 404 Not Found
Content-Type: text/html; charset=utf-8
Content-Length: 132
Connection: close

<!doctype html>
<html lang="en">
<title>Not found</title>
<h1>Not found</h1>
<p>No boat by that name is in the harbour.</p>
</html>
```

## Mistakes

::: warning Mistake: returning a view of a buffer that is about to go
A parsed request is views into the bytes that were read. A function that reads into a local buffer and returns
such a view returns a borrow of memory that ends with the function.
:::

<span class="listing-caption"><strong>Listing 21-15</strong>, <code>main.cee</code>: The request line is a view into a local buffer. This does not compile.</span>

<!-- ceesafe: fail N0244 -->
```ceesafe
fn requestLine(stream: borrow mut TcpStream) -> StringView {
    var buffer = [0u8; 512];
    let n = stream.read(mut buffer[..])!;
    let text = StringView.fromUtf8(buffer[0..n]) ?? "";
    return text.split("\r\n").next() ?? "";
}

fn main() {
    let listener = TcpListener.bind("127.0.0.1", 0)!;
    var client = TcpStream.connect("127.0.0.1", listener.port()!)!;
    client.write("GET / HTTP/1.1\r\n\r\n".bytes())!;
    var server = listener.accept()!;
    Console.writeLine(requestLine(mut server));
}
```

```text
main.cee:5:5: error[N0244]: cannot return a reference to 'buffer', which is local to 'requestLine'
  |
5 |     return text.split("\r\n").next() ?? "";
  |     ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
  = note: borrowed here (4:36)
```

Keep the bytes in the function that uses the request, as `handle` does: `head` lives there, the request is parsed,
routed and dropped inside `answer`, and only the `Response<static>` comes out.

::: warning Mistake: an `embedText` path from the package's directory
The path is relative to the source file, not to `ceesafe.toml` and not to where the compiler is run. From
`src/http.cee`, the directory `pages` is `../pages`.
:::

<span class="listing-caption"><strong>Listing 21-16</strong>, <code>main.cee</code>: A path that leads to no file. This does not compile.</span>

<!-- ceesafe: fail N0362 -->
```ceesafe
const PAGE: StringView = embedText("pages/index.html");

fn main() {
    Console.writeLine(PAGE.length);
}
```

```text
main.cee:1:36: error[N0362]: cannot embed 'pages/index.html': no file is there, or it is outside the source file's directory (a file given alone embeds only below it)
  |
1 | const PAGE: StringView = embedText("pages/index.html");
  |                                    ^^^^^^^^^^^^^^^^^^
```

::: warning Mistake: trusting the request
Everything in a request was chosen by someone else: its length, its bytes, its path. The parser checks the shape
of each line, `readHead` limits the size, `fromUtf8` refuses bytes that are not text, and `route` compares the path
with a fixed list. Indexing and slicing are checked too, so a wrong position is a panic and never a read of other
memory, but a panic still ends the program: parse with functions that return optionals, and handle the `null`.
:::

::: details Coming from Rust
`Request<r>` is `Request<'a>` with `&'a str` fields, and `embedText` is `include_str!`. A Rust server would often
reach for the `httparse` crate at this point. There is no HTTP parser in the CeeSafe standard library.
:::

::: details Coming from Go, Python or JavaScript
There, `request.path` is a new string that the garbage collector keeps alive. Here the request is a set of views
into one buffer, nothing is copied, and the compiler checks that the buffer outlives every view (N0244).
:::

## Summary

- Keep pure code (parse, route, render) apart from the edges (read, write). Pure code is tested with string
  literals, with no socket.
- A `Request<r>` holds views into the text it was parsed from. `split`, `splitOnce` and `trim` cut the text
  without copying.
- A request that cannot be parsed gets a response, 400. A path that is not known gets 404.
- `embedText(path)` puts a file's text in the program at compile time. The path is relative to the source file
  (`N0362`).
- A response built from literals and embedded files is a `Response<static>`, and borrows nothing from the request.
- A view cannot outlive the buffer it looks into (`N0244`).
- Limit what you read from a client, and check all of it.

Next: [a pool of threads](./ch21-03-thread-pool), so that one slow client does not hold up the others.
