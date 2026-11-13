---
title: Async
summary: The opening of the chapter on async code - what a task is, when tasks serve better than threads, the promise that every task has returned before the code that started it goes on, and the five sections that teach it.
assumes: [ch09-02-result, ch04-02-borrowing]
keywords: [async, await, task, structured concurrency, scope, cancellation, timeout, executor]
specification: ["type-system.md §40"]
---

# Async

**This chapter teaches** how one thread does many things that wait: timers, files, sockets, other tasks. It assumes
you can return a `Result` and pass an error on with `?`
([Recoverable errors with `Result`](./ch09-02-result)) and know what a borrow is
([Borrowing](./ch04-02-borrowing)). It uses closures ([Closures](./ch13-01-closures)), and in its last section threads
([Threads](./ch16-00-threads)).

A program that talks to a network spends most of its time waiting. A thread for each thing it waits for works, and
[Threads](./ch16-00-threads) shows how, but a thread costs a stack and a switch through the operating system, and
a server may wait for ten thousand things. A **task** is cheaper: it is a function call that can stop where it has to wait and let
another task use the thread. An `async fn` is a function whose call is a task, and `await` runs a task and gives
its result.

CeeSafe's tasks are **structured**. A task is started by `await`, by `Async.run`, or as a child of a scope, and in
each case the task has an owner that waits for it: the code that awaits it does not go on until it has returned,
and a scope does not return until every child it started has. There is no task that runs loose in
the background, and no value that stands for a task still running. Three things follow, and the chapter comes back
to each:

- a task may borrow the locals of the function that started it, as any function call may;
- an error in one task reaches the code that started it, as a `Result`, and stops the tasks beside it;
- a task that is no longer wanted is **cancelled**, which makes its next wait return an error. It is never torn
  down in the middle of a statement, so its cleanup is ordinary code.

Use tasks for work that waits, and many of them. Use threads for work that computes. The last section
shows the two together.

## The sections

1. [Tasks and await](./ch17-01-tasks-and-await): `async fn`, `await`, `Async.run`, and what runs when.
2. [Scopes](./ch17-02-scopes): tasks that run at the same time, with `Async.scope` and `s.spawn`, and task sets
   for tasks started as a program goes.
3. [Timeouts, races and cancellation](./ch17-03-timeouts-races-cancellation): `Async.timeout`, `Async.race`,
   `Async.all`, what a cancelled task sees and what it may still do.
4. [Async I/O](./ch17-04-async-io): timers, the console, files, sockets and channels for tasks, with a TCP echo
   server and its client in one program.
5. [Tasks and threads together](./ch17-05-tasks-and-threads): blocking calls, `Async.blocking`, and a scope whose
   children run on several threads.

Every listing in this chapter ends by itself and prints the same lines on every run. Where a listing waits, the
waits differ by so much that their order is not in doubt, and `ceesafec run --interp`, which the book's listings are
checked with, runs timers on a clock of its own that does not wait at all.
