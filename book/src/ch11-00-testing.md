---
title: Writing tests
summary: What chapter 11 covers - tests as functions marked @test, the checks of the library, the ceesafec test command and its outcomes, where tests live in a package, and the sweeps that fail every allocation in turn.
assumes: [ch01-03-hello-package, ch09-02-result]
keywords: [test, "@test", "ceesafec test", Check, TestFailure, leak, sweep]
specification: ["tooling.md §10"]
---

# Writing tests

The compiler proves a great deal about a CeeSafe program: that no value is used after it is gone, that no borrow
outlives what it borrows, that every error is handled or passed on. It cannot prove that the program does what you
meant. A function that should add and instead subtracts compiles without a diagnostic. A **test** is a function that
runs part of your program and checks the answer, and `ceesafec test` runs every test of a package and reports each
one.

Testing is part of the language and of the compiler. There is nothing to install and no framework to choose:

- you mark a function `@test`;
- the library has the checks (`Check.equalU64`, `Check.equalText` and a few more);
- `ceesafec test` builds the tests, runs each one as a process of its own, and reports it as passed, failed,
  panicked, timed out or leaked.

The last outcome is CeeSafe's own. The language has no garbage collector, so memory that is still allocated when a
test ends is a bug, and the command counts it. The command can also run each test again with every allocation
failing in turn. That finds the mistakes in error handling which an ordinary run never reaches, because an ordinary
run never runs out of memory.

The chapter assumes you can make a package ([Hello, package](./ch01-03-hello-package)) and know `Result` and `?`
([chapter 9](./ch09-02-result)). Its examples are four small packages about lap times on a running track.

## The sections

- [11.1 How to write tests](./ch11-01-writing-tests): the shape of a test, what makes it pass or fail, the checks,
  and how to test a function that returns a `Result`.
- [11.2 Running them](./ch11-02-running-tests): `ceesafec test`, what it prints, its options, the outcomes, leaks
  and the exit status.
- [11.3 Organising them](./ch11-03-organising-tests): tests beside the code and in the `tests` directory, what the
  tests of a package can see, tests through a library's public interface, and the two sweeps.

[Chapter 12](./ch12-00-command-line-project) then builds a command-line program, and
[section 12.4](./ch12-04-test-first) writes its tests before its code.
