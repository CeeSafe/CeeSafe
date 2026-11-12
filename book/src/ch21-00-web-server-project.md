---
title: A final project - a web server
summary: The opening of the book's last project - harbour, a small HTTP server built in five stages, from one connection at a time to a pool of threads, then async tasks, then a shutdown that lets open connections finish.
assumes: [ch12-00-command-line-project, ch16-02-channels, ch17-02-scopes]
keywords: [project, web server, HTTP, TCP, harbour, TcpListener, thread pool, async, graceful shutdown]
---

# A final project: a web server

**This chapter builds** a web server, and uses the second half of the book to do it. It assumes the first project
([A project: a command-line program](./ch12-00-command-line-project)), channels between threads
([Channels](./ch16-02-channels)) and async scopes ([Scopes](./ch17-02-scopes)).

The program is called `harbour`. It listens on a port of your own machine, and answers a browser, or the
command-line tool `curl`, with one of a few pages:

```sh
curl -i http://127.0.0.1:7878/
```

```text
HTTP/1.1 200 OK
Content-Type: text/html; charset=utf-8
Content-Length: 146
Connection: close

<!doctype html>
<html lang="en">
<title>Harbour</title>
<h1>The harbour is open.</h1>
<p>Read <a href="/about">about this server</a>.</p>
</html>
```

A web server is a good last project for the reason a command-line tool was a good first one: it is small, and it
meets the world at every edge. Bytes arrive in pieces, from clients that may be slow, wrong or gone. Many clients
come at once. And one day the server must stop without dropping the answers it is in the middle of. Each of those
is a place where the language's checks do real work: ownership decides who closes a connection, `Send` decides what
may cross to a worker thread, and the compiler refuses a blocking call in a task.

The standard library has TCP sockets and no HTTP: the project reads and writes the protocol by hand. That is the
point of the chapter, not a recommendation. The server speaks only as much HTTP as the chapter needs.

## Two protocols, briefly

**TCP** carries bytes between two programs, in order, in both directions. A server **binds** a **listener** to an
address and a port, and **accepts** connections from it. Each connection is a **stream**. The address `127.0.0.1`
is the **loopback** address: it names your own machine, and nothing sent to it leaves.

**HTTP** is text sent over such a stream. The client sends a **request**. Its head is lines that end with a carriage
return and a line feed (`\r\n`), and the head ends with an empty line:

```text
GET /about HTTP/1.1
Host: 127.0.0.1:7878
Accept: */*

```

The first line is the **request line**: a method, a path and a version. The others are **headers**, each a name, a
colon and a value. The server sends back a **response** of the same shape: a **status line** (`HTTP/1.1 200 OK`),
headers, an empty line, and then the **body**, which is the page.

## What the project draws on

| The program needs | The chapter that teaches it |
|---|---|
| a package of two files, with tests | [chapter 7](./ch07-00-packages), [chapter 11](./ch11-00-testing) |
| structs and enums for a request, a response and the errors | chapters 5 and 6 |
| views into the request's text, with a region parameter | [section 10.3](./ch10-03-regions) |
| `Result`, `?` and `@from` | [chapter 9](./ch09-00-error-handling) |
| values that close what they own when they are destroyed | [chapter 15](./ch15-00-memory) |
| threads and a channel | [chapter 16](./ch16-00-threads) |
| tasks, a task set, a race, a shield | [chapter 17](./ch17-00-async) |

New in this chapter are the library's blocking sockets, `TcpListener` and `TcpStream`, the built-in function
`embedText`, and `Console.flush`.

## The sections

1. [One connection at a time](./ch21-01-one-connection): a listener, a loop that accepts, and a fixed page for
   every request.
2. [Requests and responses](./ch21-02-requests-and-responses): parsing the request, choosing a page by its path,
   and a 404.
3. [A pool of threads](./ch21-03-thread-pool): a fixed number of worker threads that take connections from a
   channel.
4. [Async tasks](./ch21-04-async-tasks): the same server with a task for each connection, on one thread.
5. [Stopping gracefully](./ch21-05-graceful-shutdown): no new connections, and the open ones are answered first.

## The package

```sh
ceesafec new harbour
cd harbour
```

The finished package is in the book's repository at `book/projects/ch21/harbour`, and the program as it stands at
the end of the first four sections is beside it, in `stage1` to `stage4`.

Every stage has a test that starts the server inside the test, on a port the system chooses, makes requests from
the same program, checks the answers and stops the server. So `ceesafec test` checks the whole server, sockets
included, and ends by itself.

::: info A note on running it
The server runs until you stop it, so build it and run the executable:

```sh
ceesafec build
./out/harbour
```

It prints `listening on http://127.0.0.1:7878`. Open that address in a browser, or use `curl` in a second terminal.
Stop the server with Ctrl+C. If the port is in use on your machine, change the number in `main`.
:::

Next: [one connection at a time](./ch21-01-one-connection).
