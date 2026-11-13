---
title: Glossary
summary: Every term the book uses in a technical sense, in alphabetical order, each with one definition and the section that introduces it, and with the words of other languages pointing to CeeSafe's own term.
keywords: [glossary, term, definition, borrow, reference, pointer, trait, lifetime, region, interface, ownership]
---

# Glossary

This page defines every term the book uses in a technical sense, in alphabetical order. Each row stands alone: the
term, what it means in CeeSafe, and the section that introduces it. A word from another language that CeeSafe does
not use, such as "trait" or "lifetime", has a row that points to CeeSafe's term, in italics. So has a word that
the specification or the diagnostics use where the book uses another, such as "reference", "loan" or "dropped".
The specification is the formal text: its noun for a borrow is "reference", and its rules say "mutable" where the
book says "exclusive".

A few rows name a term of the specification that no chapter teaches. Their last column links to the
specification's section on the documentation site.

Where one word has two meanings in the book, each meaning has its own row, with the meaning in parentheses after
the word. Names of the library, such as `Vector` and `Mutex`, are here only when the book uses them as terms.

## A

| Term | Meaning | Section |
|---|---|---|
| allocation | Asking for memory while the program runs. It can fail, so an operation that allocates returns `Result<…, AllocError>`. | [9.3](./ch09-03-allocation-failure) |
| allocation report | The line the runtime writes for a test, with the number of allocations the test made and the number still live when it ended. | [11.2](./ch11-02-running-tests) |
| allocator | Where a box's or a collection's memory comes from. It is the last type parameter of the type: `Vector<T>` is short for `Vector<T, SystemHeap>`. | [15.3](./ch15-03-allocators) |
| arena | An allocator that takes memory in large chunks, hands out pieces of them, and frees everything in one step. An allocation from it is a borrow of the arena. | [15.3](./ch15-03-allocators) |
| argument | The value a call gives for one parameter, in order: the `3` in `area(3, 4)`. | [3.3](./ch03-03-functions) |
| argument, command-line | A word typed after a program's name when it is started. `Process.argument(i)` returns it as an optional. | [12.1](./ch12-01-arguments) |
| arm | One case of a `match`: a pattern, then `=>`, then a block or an expression. | [6.2](./ch06-02-match) |
| array | A fixed number of values of one type. Its type is `[T; N]`, and the length is part of the type. | [3.2](./ch03-02-data-types) |
| associated constant | See *interface constant*. | [20.3](./ch20-03-more-interfaces-and-generics) |
| associated function | CeeSafe's term is *static function*. | [5.3](./ch05-03-methods) |
| associated type | A type that an interface declares by name and that each `impl` chooses. | [20.3](./ch20-03-more-interfaces-and-generics) |
| async closure | A closure written `async () => body`, whose call is a task. A scope starts its children from async closures. | [17.2](./ch17-02-scopes) |
| async function | A function declared `async fn`. Its call is a task, and its body may use `await`. | [17.1](./ch17-01-tasks-and-await) |
| `async move` | An async closure written `async move () => body`, which owns what it captures. A child of a task set must be one, and a child of a scope, of `Async.race`, of `Async.all` or of `Async.timeout` may be one. | [17.2](./ch17-02-scopes) |
| atomic | An integer or a `bool` whose every operation happens as one step that no other thread can interrupt, such as `AtomicU64`. | [16.3](./ch16-03-shared-state) |
| attribute | A name after `@`, written before a declaration, that changes how the compiler treats it: `@test`, `@derive(Equal)`, `@export("name")`. | [20.5](./ch20-05-derives-and-macros) |
| await | The operator that runs a task and gives its result: `await f()`. It is the only place where a task can be suspended. | [17.1](./ch17-01-tasks-and-await) |

## B

| Term | Meaning | Section |
|---|---|---|
| binding | Giving a name in a pattern the part of the value that fits there. The name can be used in the arm, or after the `let`. | [6.2](./ch06-02-match), [19.1](./ch19-01-where-patterns-go) |
| block (a thread) | To keep a thread while waiting, so that nothing else can run on it. A task must never block. It waits with `await`. | [17.4](./ch17-04-async-io) |
| block (of code) | Statements between `{` and `}`. A block is a scope, and as the branch of an `if` or `match` expression it can end with its value. | [3.5](./ch03-05-control-flow) |
| borrow | Permission to use a value without owning it: a *shared borrow*, written `borrow T`, or an *exclusive borrow*, written `borrow mut T`. The compiler checks that the value outlives it. The specification and the diagnostics call a borrow a "reference". | [4.2](./ch04-02-borrowing) |
| bound | A requirement on a type parameter, written after a colon: `T: Copy`, `T: Shape`. Generic code may do with a `T` only what its bounds allow. | [10.1](./ch10-01-generic-types), [10.2](./ch10-02-interfaces) |
| boundary | The places where a program meets what it does not control: input, files, arguments, the network. Failures there are expected failures. | [9.4](./ch09-04-to-panic-or-not) |
| bounds check | The test, made at each index, that the index is less than the length. A failed one is a panic. | [4.3](./ch04-03-slices-and-views), [9.1](./ch09-01-panics) |
| box | A `Box<T>`: the owner of one value on the heap. Recursive types and `dyn` values that are owned need one. | [15.1](./ch15-01-box) |

## C

| Term | Meaning | Section |
|---|---|---|
| C string literal | Text written `c"…"`. The compiler adds the zero byte that ends a C string. | [20.2](./ch20-02-calling-c) |
| callback | A CeeSafe function that C code calls through a function pointer. `@callback` before the function gives it a C address, and its name is then a value of the type `extern "C" fn(…)`. | [20.2](./ch20-02-calling-c) |
| cancellation | Telling a task that it is no longer wanted. Its next wait returns the error `Cancelled`, and it ends by returning. | [17.3](./ch17-03-timeouts-races-cancellation) |
| cancellation point | A place where a task can notice that it was cancelled: one of the library's waits, such as a sleep, a read, a write or a channel receive, or a call of `Async.checkpoint`. | [17.3](./ch17-03-timeouts-races-cancellation) |
| capture | A variable from around a closure that the closure's body uses. A closure literal borrows its captures, exclusively those that its body changes, and a `move` literal owns them. | [13.1](./ch13-01-closures) |
| cast | CeeSafe has none that cuts bits off. See *conversion*. | [3.2](./ch03-02-data-types) |
| catch-all | An arm whose pattern fits every value, `_` or a name. It must be the last arm. | [6.2](./ch06-02-match), [19.2](./ch19-02-patterns-that-can-fail) |
| cell | A value that can be changed through a shared borrow. `Cell<T>` copies a Copy value in and out, `RefCell<T>` lends its value through guards and panics where two accesses conflict, and `OnceCell<T>` is written once. A cell is a move type, and it is not `Share`. | [15.4](./ch15-04-cells) |
| `cfg` block | A block that is compiled only in some builds, chosen by a setting of the build. | [14.1](./ch14-01-build-settings) |
| channel | A queue that one thread or task puts values into and another takes them out of. It is bounded: it never holds more than its capacity. | [16.2](./ch16-02-channels) |
| check (in a test) | A function of the library that compares two values in a test, such as `Check.equalU64`. The test fails when a check does. | [11.1](./ch11-01-writing-tests) |
| check (the command) | `ceesafec check`: parse, type-check and borrow-check a package without building it. | [1.3](./ch01-03-hello-package) |
| checkpoint | `Async.checkpoint(error)`: a call that returns `Err(error)` if the current task is cancelled, and `Ok()` otherwise. It never waits, and it gives a long computation a *cancellation point*. | [17.3](./ch17-03-timeouts-races-cancellation) |
| child | A task started by a scope or a task set, or as one of the two closures of `Async.race` or `Async.all`. What started it does not return before the child has. | [17.2](./ch17-02-scopes) |
| class | CeeSafe has none. See *struct*, *method* and *interface*. | [5.1](./ch05-01-defining-structs) |
| closure | A function written where it is used, such as `(v) => v > limit`, that uses variables around it, its captures. Its type is `borrow fn(…)`, or `borrow mut fn(…)` if it changes a capture. A literal that captures nothing is a *function value*. | [13.1](./ch13-01-closures) |
| closure literal | The written form of a closure, `(x) => x + 1`. It is a borrow of its environment, so making one allocates nothing, and it cannot be returned or used after the block it is written in. | [13.1](./ch13-01-closures) |
| collection | A value that holds a number of values decided while the program runs: a vector, a string, a map. It keeps its elements on the heap and owns them. | [8](./ch08-00-collections) |
| compiler | `ceesafec`, the program that checks CeeSafe and compiles it to C. | [1.1](./ch01-01-installation) |
| const parameter | A generic parameter that stands for a value, not a type, such as the length of an array. | [20.3](./ch20-03-more-interfaces-and-generics) |
| constant | A name declared with `const` for a value that the compiler computes. It never changes. | [3.1](./ch03-01-variables) |
| constructor | CeeSafe has no special one. See *static function*. | [5.3](./ch05-03-methods) |
| consume | To take a value by value, so that it cannot be used afterwards: a `match` on a move type consumes it, and `join` consumes a thread's handle. | [19.3](./ch19-03-the-syntax-whole) |
| context | Where kernel code runs, which limits what it may do. `@context(irq)` marks an interrupt handler, and the compiler checks that nothing it calls sleeps or allocates. See also *unsafe context*. | [22.2](./ch22-02-kernel-code) |
| conversion | Changing a value's type. `as` converts only when nothing can be lost. `truncate`, `checkedTruncate` and `saturatingTruncate` say what happens when the value does not fit. | [3.2](./ch03-02-data-types), [20.4](./ch20-04-arithmetic) |
| Copy type | A type whose values are copied when assigned or passed, so the original stays usable: integers, floats, `bool`, shared borrows, `Slice<T>`, `StringView`, function values and raw pointers, and an array, an optional, an enum or a struct whose parts are all Copy. A struct with a `drop` block or marked `@noncopy` is not Copy. | [4.1](./ch04-01-what-ownership-is) |
| crate | CeeSafe's term is *package*. | [7.1](./ch07-01-package-and-manifest) |
| cursor | A small value that names a position in an ordered map, used to visit its entries in order from a key. | [8.3](./ch08-03-maps) |

## D

| Term | Meaning | Section |
|---|---|---|
| data race | Two threads using the same memory at the same time, at least one of them writing. A CeeSafe program that compiles has none outside unsafe code. | [16](./ch16-00-threads), [16.4](./ch16-04-send-and-share) |
| deadlock | Two threads or tasks each waiting for something the other holds, so that neither goes on. The compiler does not prevent it. | [16](./ch16-00-threads), [16.3](./ch16-03-shared-state) |
| debug assertion | A check written `debugAssert(condition, message)`. A debug build compiles it, and it panics when the condition is false. Other builds compile nothing of it. | [14.1](./ch14-01-build-settings) |
| deep result | A result that may hold what a borrowed parameter's value holds, but not a borrow of that value itself: `fn token(borrow mut this) -> StringView<r>` in a `struct Lexer<region r>` returns a view of the text the lexer views. So the caller may use the lexer again while the result is live. | [`type-system.md` §25.6](https://ceesafe.github.io/CeeSafe/reference/type-system) |
| default method | A method that has a body in its interface. An `impl` that leaves it out gets that body. | [10.2](./ch10-02-interfaces) |
| `defer` | A statement, `defer s;`, that runs `s` when the enclosing block ends on any path: at its end, or at a `break`, a `continue` or a `return` that leaves it. Deferred statements and the destruction of locals run in the reverse of the order they are written in. | [15.2](./ch15-02-destruction) |
| definite initialisation | The compiler's check that a variable has a value on every path that reaches a read, a borrow or a move of it. A `var` that is declared without a value and assigned in only one branch of an `if` fails it (N0212), and so does a variable that may have been moved (N0250). | [3.1](./ch03-01-variables), [4.1](./ch04-01-what-ownership-is) |
| dependency | A package that another package uses and names in its manifest, by a path, a git repository or a version requirement. | [7.3](./ch07-03-imports), [7.4](./ch07-04-dependencies) |
| derive | To have the compiler write an `impl`: `@derive(Equal, HashKey, OrderKey)` before a struct or an enum. | [20.5](./ch20-05-derives-and-macros) |
| destroy | To end a value: its *destructor* runs and its memory is given back. A value is destroyed exactly once: when its owner's scope ends, when its owner is assigned a new value, or, for a temporary, at the end of its statement. A value that was moved is destroyed by its new owner. | [4.1](./ch04-01-what-ownership-is), [15.2](./ch15-02-destruction) |
| destruction | The end of a value: its `drop` block runs, its fields are destroyed, and its memory is given back. The compiler puts it where the value's owner ends. See *destroy*. | [4.1](./ch04-01-what-ownership-is), [15.2](./ch15-02-destruction) |
| destructor | What runs when a value is destroyed: the type's *`drop` block*, if it has one, and then the destruction of its fields. A program cannot call it. | [15.2](./ch15-02-destruction) |
| diagnostic | A message from the compiler about the source: an error, which stops the build, or a warning. Each has a code, such as `N0240`. | [Introduction](./ch00-00-introduction) |
| discard | To throw a value away without using it: a call statement whose result nothing reads, or `let _ = e;`, which says so in writing. A discarded value is destroyed at the end of its statement, and the compiler warns when a statement discards a `Result` without saying so (N0402). | [9.2](./ch09-02-result), [15.2](./ch15-02-destruction) |
| documentation comment | A comment that starts `///` and describes the declaration after it. `ceesafec doc` makes a page from them. | [3.4](./ch03-04-comments) |
| drop | As a verb, the specification's and the compiler's word for *destroy*. `drop` is also the keyword of a *`drop` block*. | [15.2](./ch15-02-destruction) |
| `drop` block | Code in a struct, written `drop { … }`, that runs once when a value of the struct is destroyed, before its fields are destroyed. A struct with one is a move type. An enum cannot have one. | [15.2](./ch15-02-destruction) |
| drop flag | A hidden flag that the compiler keeps beside a variable whose value is moved on some paths and not on others. Where the variable's scope ends, the value is destroyed only if the flag says that it is still there, so it is destroyed exactly once. | [15.2](./ch15-02-destruction) |
| dropped | The specification's and the compiler's word for destroyed. See *destroy*. Of a `Result` or another value that is thrown away without being read, the book says *discard*. | [15.2](./ch15-02-destruction) |
| `dyn fn` | `dyn fn(T) -> R`: the `dyn` type of a closure that owns its captures, and `dyn mut fn(T) -> R` of one that changes them. It is used in a box: see *owning closure*. | [13.1](./ch13-01-closures) |
| `dyn` type | `dyn I`: a value whose own type is not known where it is used, only the interface `I` that it implements. It is used behind a borrow or in a box. | [18.1](./ch18-01-dynamic-dispatch) |
| dynamic dispatch | A call of a method through a `dyn` value. The function is found while the program runs, in the value's table. | [18.1](./ch18-01-dynamic-dispatch) |
| dyn-compatible | Said of an interface that has a `dyn` type: every method takes `borrow this` or `borrow mut this` and has no type parameters, and the interface has no static function, associated type or constant. | [18.1](./ch18-01-dynamic-dispatch) |

## E

| Term | Meaning | Section |
|---|---|---|
| effect | Something a function may do that a context forbids: block its thread (`sleep`), allocate (`alloc`) or suspend its task (`suspend`). The compiler works it out for CeeSafe code, and `@effects(…)` states it for a C function. | [22.2](./ch22-02-kernel-code) |
| effect list | `@effects(…)` written before `fn` in the type of a function value or a closure. It names the *effects* a call through the value may have: `@effects() fn(i32) -> i32` accepts only a function that neither blocks nor allocates (N0295 otherwise). A type without a list is taken to do both, so code that must not block cannot call through it. | [`type-system.md` §36.3](https://ceesafe.github.io/CeeSafe/reference/type-system) |
| elision rule | The rule that a function with exactly one parameter that can hold a borrow need not say where its result borrows from: it borrows from that parameter. With several such parameters, the result names its sources, as `borrow(a, b) T` or with a region (N0245 otherwise). | [10.3](./ch10-03-regions) |
| enum | A type whose value is exactly one of a fixed list of variants. A variant can carry a payload. | [6.1](./ch06-01-defining-an-enum) |
| environment (freestanding) | The code that surrounds a freestanding object and starts it: a kernel, a boot loader, a board's start-up file. | [22.1](./ch22-01-the-freestanding-profile) |
| environment (of a closure) | The hidden value a closure is made of, which holds the borrows, or for a `move` literal the values, of what it captures. | [13.3](./ch13-03-project-improved), [13.4](./ch13-04-what-they-cost) |
| environment variable | A named text that a program inherits from whatever started it. `Environment.variable(name)` returns it as an optional. | [12.5](./ch12-05-environment) |
| error | Either a *diagnostic* of the compiler that stops the build, or the value in the `Err` of a *`Result`* while the program runs. | [9.2](./ch09-02-result) |
| exception | CeeSafe has none. A bug is a *panic*, and an expected failure is a *`Result`*. | [9](./ch09-00-error-handling) |
| exclusive borrow | A `borrow mut T`: a borrow that can read and write. While it is live, the value it borrows cannot be used in any other way. The argument is written `mut x`, and passing one on makes a *reborrow*. The diagnostics say "mutably borrowed". | [4.2](./ch04-02-borrowing) |
| exclusive reference | The book's term is *exclusive borrow*. | [4.2](./ch04-02-borrowing) |
| executable | A package whose kind is `executable`: a program with a `main`. Also the file that `ceesafec build` makes from it. | [7.1](./ch07-01-package-and-manifest) |
| executor | The part of the library that chooses which task runs next and waits on the operating system when none can run. | [17.5](./ch17-05-tasks-and-threads) |
| exhaustive | Said of a `match` in which every possible value fits some arm. A `match` must be exhaustive. | [6.2](./ch06-02-match) |
| exit status | The small number a program gives, when it ends, to the program that started it. 0 means success, and a panic gives 134. | [12.1](./ch12-01-arguments), [9.1](./ch09-01-panics) |
| expected failure | A failure that can happen when every line of the program is correct, such as a missing file. A function reports it as a `Result`. | [9.4](./ch09-04-to-panic-or-not) |
| export | To give a function a name in C, with `@export("name")`, so that a host program can call it. | [14.3](./ch14-03-libraries-for-other-programs) |
| expression | Code that computes a value: `3`, `width * height`, a call. | [3.3](./ch03-03-functions) |
| `extern "C"` block | A list of declarations of C functions and data. A call of one is allowed only in an unsafe context. | [20.2](./ch20-02-calling-c) |

## F

| Term | Meaning | Section |
|---|---|---|
| field | One named value in a struct, with its type. It is reached as `value.field`, and in a method as `this.field`. | [5.1](./ch05-01-defining-structs) |
| fit | A value fits a pattern or does not. When it fits, the names in the pattern bind parts of it. | [6.2](./ch06-02-match), [19.1](./ch19-01-where-patterns-go) |
| flow | What a function's signature says about borrows: that the result may hold a borrow taken from a parameter, or that a `borrow mut` parameter may store what another parameter holds. The compiler holds the body to the function's flows, and at a call it keeps the arguments that a flow names borrowed for as long as the result is used. `borrow(a, b) T`, a shared region name and the *elision rule* each state a flow. | [10.3](./ch10-03-regions) |
| fork-join | Running two closures at the same time and going on only when both have ended: `Thread.both(a, b)` runs `a` on a new thread and `b` on the calling one. The closures may borrow the caller's variables, because the call returns only after both. | [16.1](./ch16-01-starting-threads) |
| frame (of a task) | The block of memory, of a size the compiler knows, that holds a task's parameters and the locals it needs across a wait. | [17.1](./ch17-01-tasks-and-await) |
| frame (scratch) | See *scratch frame*. | [15.3](./ch15-03-allocators) |
| frame stack | The memory from which a task takes the frame of an async function that awaits itself, directly or through other functions. A frame cannot contain itself, so such a frame is not inside the frame of the task that awaits it. The frame stack grows as a call stack does. | [17.1](./ch17-01-tasks-and-await) |
| freestanding | The profile in which nothing is there but the processor and memory: no operating system and no C library. | [22.1](./ch22-01-the-freestanding-profile) |
| free-standing type | The specification's word for a type that is not a *scoped type*: no value of it can hold a borrow, so a value can be kept for any time, for example in a box that a function returns. `String` and `Vector<u8>` are free-standing, and `StringView` is not. It has nothing to do with the *freestanding* profile. | [`ownership.md` §1](https://ceesafe.github.io/CeeSafe/reference/ownership) |
| function pointer | For CeeSafe code see *function value*. C's function pointer has the type `extern "C" fn(…)`, and a `@callback` function is a value of it. | [20.2](./ch20-02-calling-c) |
| function value | A function used as a value: passed, kept in a variable or in a field. Its type is `fn(T) -> R`. It captures nothing, it is Copy, and a closure literal that captures nothing is one. | [13.1](./ch13-01-closures) |
| future | CeeSafe has no value that stands for a task still running. See *task*. | [17](./ch17-00-async) |

## G

| Term | Meaning | Section |
|---|---|---|
| garbage collector | CeeSafe has none. Memory is given back when a value's owner ends. See *ownership*. | [4](./ch04-00-ownership) |
| generic | Said of a function or type that has type parameters. Each use with different type arguments is compiled as its own copy. | [10.1](./ch10-01-generic-types) |
| guard (in a pattern) | A condition after a pattern, `n if (n > 100)`. The arm is taken only when the pattern fits and the condition is true. | [6.2](./ch06-02-match), [19.2](./ch19-02-patterns-that-can-fail) |
| guard (of a lock or a cell) | A value that stands for an access, such as a `MutexGuard` or a `CellRef`. The access ends when the guard is destroyed. | [15.4](./ch15-04-cells), [16.3](./ch16-03-shared-state) |

## H

| Term | Meaning | Section |
|---|---|---|
| handle | A small value that stands for something kept elsewhere: a `Thread<T>` for a running thread, a clone of a channel for the one queue, a `Handle<T>` for a value in a `SlotMap`, a raw pointer that a host keeps. | [16.1](./ch16-01-starting-threads), [15.5](./ch15-05-shared-ownership) |
| heap | Memory that stays until it is freed, wherever the function that asked for it has gone. A `String`, a `Vector` and a `Box` keep their contents there. | [4.1](./ch04-01-what-ownership-is) |
| host | A program in another language that calls CeeSafe through exported functions. | [14.3](./ch14-03-libraries-for-other-programs), [22.3](./ch22-03-inside-a-host-program) |
| hosted | The default profile: an operating system and a C library are there, to start the program, give it memory and carry its output. | [22.1](./ch22-01-the-freestanding-profile) |

## I

| Term | Meaning | Section |
|---|---|---|
| `impl` | A block that implements an interface for a type, `impl Shape for Square { … }`, giving each method a body. An `impl` block without an interface adds methods to a struct or an enum. Where it implements an interface, a method's result may borrow from its parameters only as the interface's method says. | [5.3](./ch05-03-methods), [10.2](./ch10-02-interfaces) |
| import | A line that lets a file use a dependency's items by their plain names: `import tally.Tally;` for one item, `import tally;` for every public one. | [7.3](./ch07-03-imports) |
| index (of an element) | The position of an element in an array, a slice or a vector. It starts at 0 and its type is `usize`. | [3.2](./ch03-02-data-types) |
| index (of packages) | A git repository that lists the published versions of each library and where each is. The registry is one. | [7.4](./ch07-04-dependencies), [14.2](./ch14-02-publishing) |
| inference | The compiler finding a type that is not written: a variable's type from its first value, a call's type arguments from its arguments. | [3.1](./ch03-01-variables), [10.1](./ch10-01-generic-types) |
| inheritance | CeeSafe has none between structs. An interface can require another: see *supertrait*. | [20.3](./ch20-03-more-interfaces-and-generics) |
| interface | A named list of methods. A type implements it with an `impl`, and a bound `T: I` accepts only the types that do. | [10.2](./ch10-02-interfaces) |
| interface constant | A constant that an interface declares, with or without a default value, and that each `impl` can set. | [20.3](./ch20-03-more-interfaces-and-generics) |
| interior mutability | The specification's name for changing a value through a shared borrow. See *cell*. | [15.4](./ch15-04-cells) |
| internal | Said of an item without `public`: only its own package can use it, in any of its files. | [7.2](./ch07-02-files-and-public) |
| interpreter | The part of the compiler that runs a program directly, with no C compiler: `ceesafec run --interp`. | [1.2](./ch01-02-hello-world) |
| item | Something declared at the top of a file: a function, a struct, an enum, an interface, a constant. | [7.2](./ch07-02-files-and-public) |
| iterator | Any value with a method `next` that gives the next value or `null`. `for` loops over one. | [13.2](./ch13-02-iteration) |

## L

| Term | Meaning | Section |
|---|---|---|
| lambda | CeeSafe's term is *closure*. | [13.1](./ch13-01-closures) |
| leak | Memory that was allocated and never freed. `ceesafec test` reports a test that leaves some as `leaked`. A cycle of `Arc` owners makes one. | [11.2](./ch11-02-running-tests), [15.5](./ch15-05-shared-ownership) |
| library | A package whose kind is `library`: it has no `main`, and other packages depend on it. | [7.1](./ch07-01-package-and-manifest) |
| lifetime | CeeSafe's terms are *live*, for how long a borrow lasts, and *region*, for the name of what a borrow borrows from. | [10.3](./ch10-03-regions) |
| listener | A socket bound to an address and a port, from which a server accepts connections: a `TcpListener`. | [21.1](./ch21-01-one-connection) |
| listing | A numbered example in this book. It is a whole program, shown with what it prints or the diagnostic it gets. | [Introduction](./ch00-00-introduction) |
| literal | A value written out in the source: `42`, `2.5`, `true`, `"text"`. In a pattern, only that value fits it. | [3.2](./ch03-02-data-types), [6.2](./ch06-02-match) |
| live | Said of a borrow from where it is made to the last place where it, or anything made from it, is used. It is not live to the end of its block. | [4.2](./ch04-02-borrowing) |
| loan | The specification's and the diagnostics' word for one borrowing of a value, which lasts while the borrow is *live*. A type that "holds a loan" is a *scoped type*. | [4.2](./ch04-02-borrowing), [16.1](./ch16-01-starting-threads) |
| lock | See *mutex*. | [16.3](./ch16-03-shared-state) |
| lock file | `ceesafe.lock`, which `ceesafec fetch` writes. It records exactly which version of each dependency the package builds with. | [7.4](./ch07-04-dependencies) |
| loopback | The address `127.0.0.1`, which names your own machine. Nothing sent to it leaves the machine. | [21](./ch21-00-web-server-project) |

## M

| Term | Meaning | Section |
|---|---|---|
| macro | CeeSafe has none. Attributes, derives, `debugAssert`, `embed` and code generators do the work. | [20.5](./ch20-05-derives-and-macros) |
| manifest | `ceesafe.toml`, the file at a package's root that names the package, says what kind it is and lists its dependencies. | [7.1](./ch07-01-package-and-manifest) |
| map | A collection of pairs of a key and a value: `HashMap`, which has no order, and `OrderedMap`, which keeps its keys in order. | [8.3](./ch08-03-maps) |
| `match` | A statement or expression that takes a value and a list of arms, and runs the first arm whose pattern the value fits. | [6.2](./ch06-02-match) |
| method | A function declared in a struct, an enum or an `impl` whose first parameter is `this`. It is called as `value.method(…)`. | [5.3](./ch05-03-methods) |
| module | CeeSafe has none. All the files of a *package* share one scope, and another package's items are reached by a *qualified name* or an *import*. | [7.2](./ch07-02-files-and-public) |
| monomorphisation | The book says that each use of a *generic* with different type arguments is compiled as its own copy. | [10.1](./ch10-01-generic-types) |
| move | Handing a value to a new owner by assigning it, passing it or returning it. The old variable cannot be used afterwards. `move x` moves explicitly, also a value of a Copy type. | [4.1](./ch04-01-what-ownership-is), [15.2](./ch15-02-destruction) |
| `move` literal | A closure written `move (x) => body`, which owns what it captures: each capture is moved in, or copied if its type is Copy. It is written where a `Box<dyn fn(…)>` is made or as the closure of `Thread.spawn`, and it cannot capture a borrow. | [13.1](./ch13-01-closures), [16.1](./ch16-01-starting-threads) |
| move type | A type that is not Copy, so that assigning or passing a value of it moves the value: `String`, `Vector<T>`, `Box<T>`, a struct or an enum with a part of a move type, and a struct with a `drop` block or marked `@noncopy`. | [4.1](./ch04-01-what-ownership-is) |
| mutable borrow | The specification's and the compiler's word for an exclusive borrow: the diagnostics say "mutably borrowed" and "borrow as mutable". See *exclusive borrow*. | [4.2](./ch04-02-borrowing) |
| mutex | A `Mutex<T>`: a value and a lock. `lock()` waits until the lock is free and returns a guard, and the value is reached only through the guard. | [16.3](./ch16-03-shared-state) |

## N

| Term | Meaning | Section |
|---|---|---|
| namespace | CeeSafe has none. See *package* and *qualified name*. | [7.3](./ch07-03-imports) |
| needs drop | The specification's and the compiler's words for a type whose destruction runs code: a struct with a `drop` block, or a type with a part that needs drop, such as a `String`, a `Vector` or a `Box`. Every such type is a *move type*, and an arena or a scratch frame cannot hold a value of one (N0264). A move type need not need drop: a `@noncopy` struct of integers does not. | [15.3](./ch15-03-allocators) |
| `never` | The return type of a function that never returns, because it always exits or panics. The compiler knows that nothing runs after a call of it. | [3.3](./ch03-03-functions) |
| `nil`, `None` | CeeSafe's word is *`null`*. | [6.3](./ch06-03-optionals) |
| `null` | The value of an optional that holds nothing. Only an optional can be `null`. | [6.3](./ch06-03-optionals) |

## O

| Term | Meaning | Section |
|---|---|---|
| object (file) | The output of `ceesafec build --emit obj`: compiled code that a linker joins to a kernel or another program. | [14.3](./ch14-03-libraries-for-other-programs), [22.1](./ch22-01-the-freestanding-profile) |
| `Option` | CeeSafe's term is *optional*, written `T?`. | [6.3](./ch06-03-optionals) |
| optional | A type written `T?`. Its value is `Some(v)` or `null`, and the value must be taken out, with `??`, `match`, `is`, `?` or `!`, before it is used. | [6.3](./ch06-03-optionals) |
| or-pattern | Two or more patterns joined with a bar. A value fits it if it fits any of them. | [19.3](./ch19-03-the-syntax-whole) |
| orphan rule | An `impl I for T` is written in the package that declares `I` or in the package that declares `T`, and nowhere else. | [10.2](./ch10-02-interfaces) |
| overflow | A result of arithmetic that does not fit its type. The operators panic on it, and the `checked`, `saturating` and `wrapping` methods say what to do in its place. | [3.2](./ch03-02-data-types), [20.4](./ch20-04-arithmetic) |
| owner | The variable, or the other value, that holds a value. Every value has exactly one, and the value is destroyed when its owner ends. | [4.1](./ch04-01-what-ownership-is) |
| ownership | The rules by which the compiler knows, for every value, who its owner is: one owner, moves, borrows, and destruction at the owner's end. | [4](./ch04-00-ownership) |
| owning closure | A `Box<dyn fn(T) -> R>` made from a *`move` literal*: a closure that owns its captures, so it can be returned, kept in a field or a collection, and outlive the function that made it. | [13.1](./ch13-01-closures) |

## P

| Term | Meaning | Section |
|---|---|---|
| package | A directory with a manifest, `ceesafe.toml`, and source files under `src/`. It is the unit that is built, tested, depended on and published. | [1.3](./ch01-03-hello-package), [7.1](./ch07-01-package-and-manifest) |
| panic | The stop of a program at a bug: an index out of bounds, an overflow, an unwrapped `null`, a failed `assert`. It prints `ceesafe panic: …` on standard error and ends the process with exit status 134. It cannot be caught, and no `drop` block or `defer` runs after it. | [9.1](./ch09-01-panics) |
| panic hook | A function of a host program that the runtime calls when CeeSafe panics, before the process ends. | [14.3](./ch14-03-libraries-for-other-programs) |
| parameter | A name and a type in a function's declaration, `width: i32`. A call gives one argument for each. | [3.3](./ch03-03-functions) |
| partial move | Moving one field out of a struct that a variable holds. The other fields stay usable, the struct cannot be used as a whole until the field is assigned again (N0250), and a struct with a `drop` block does not allow it (N0252). | [15.2](./ch15-02-destruction) |
| pattern | A form that a value fits or does not, and that can bind names to parts of the value. Patterns go in the arms of a `match`, after `is`, and in a `let` with an `else`. | [6.2](./ch06-02-match), [19.1](./ch19-01-where-patterns-go) |
| payload | The values a variant of an enum carries: the two numbers in `Command.Move(3, 4)`. | [6.1](./ch06-01-defining-an-enum) |
| place | An expression that names where a value is kept: a variable, `this`, a field of a place, or what a borrow leads to. Only a place can be borrowed, so the result of a call is bound to a variable before it is borrowed (N0247). | [4.2](./ch04-02-borrowing) |
| pointer | In safe code, CeeSafe's term is *borrow*. An address the compiler does not check is a *raw pointer*. | [4.2](./ch04-02-borrowing), [20.1](./ch20-01-unsafe-code) |
| pool (of threads) | A fixed number of threads, started once, that take work from a channel. | [21.3](./ch21-03-thread-pool) |
| pool (of values) | A `Pool<T>`: an allocator that holds up to a fixed number of values of one type. | [15.3](./ch15-03-allocators) |
| private | Said of an item, field or method marked `private`: only the file that declares it can use it. | [7.2](./ch07-02-files-and-public) |
| profile | What a build may assume is there when the program runs: *hosted*, the default, or *freestanding*, chosen with `--profile`. | [22.1](./ch22-01-the-freestanding-profile) |
| public | Said of an item, field or method marked `public`: other packages can use it. | [7.2](./ch07-02-files-and-public) |

## Q

| Term | Meaning | Section |
|---|---|---|
| qualified name | A dependency's name, a dot, and the name of one of its public items: `tally.pair`. It needs no import. | [7.3](./ch07-03-imports) |

## R

| Term | Meaning | Section |
|---|---|---|
| race | `Async.race(a, b)`: a call that runs two async closures at the same time. The first to return decides the result: after an `Ok`, the other is cancelled and awaited before the call returns. | [17.3](./ch17-03-timeouts-races-cancellation) |
| range | `lo..hi`: the integers from `lo` up to, but not including, `hi`. A `for` loops over one, and `a[lo..hi]` takes a slice. | [3.5](./ch03-05-control-flow), [4.3](./ch04-03-slices-and-views) |
| raw pointer | An address with a type, `*T` or `*mut T`. It is Copy, may be null and may outlive its target, and the borrow rule does not see it. It is read and written only in an unsafe context. | [20.1](./ch20-01-unsafe-code) |
| reborrow | A new exclusive borrow made from an exclusive borrow. Passing an exclusive borrow to a function or giving it to a `let` makes one, because an exclusive borrow is never copied. The original cannot be used while the reborrow is live, and can be used again afterwards. | [4.2](./ch04-02-borrowing) |
| receiver | The first parameter of a method: `borrow this`, `borrow mut this`, or `this` by value. It is the value before the dot in a call. | [5.3](./ch05-03-methods) |
| reference | The specification's and the compiler's word for a borrow, a value of type `borrow T` or `borrow mut T`. See *borrow*. | [4.2](./ch04-02-borrowing) |
| reference count | The count of owners that an `Arc` keeps beside its value. See *shared ownership*. | [15.5](./ch15-05-shared-ownership) |
| refutable, irrefutable | The book says that a pattern "can fail" or "cannot fail": some value does not fit it, or every value does. A pattern that can fail needs another arm or an `else`. | [19.2](./ch19-02-patterns-that-can-fail) |
| region | A name for what a borrow borrows from, written in a type as `StringView<r>` or `borrow(r) T`, so that the compiler can check that a value of the type does not outlive it. `static` is the region of what lives as long as the program. The specification and the diagnostics also call an allocator's memory, such as an arena's, a region. | [10.3](./ch10-03-regions) |
| region parameter | A region declared by a struct, an enum or a function, before its type parameters: `struct Word<region r>`, with a field `StringView<r>`. A struct or an enum that holds a borrow, a view or a slice must declare one. | [10.3](./ch10-03-regions) |
| register | One of the few words at a fixed address through which a device shows itself to the processor. It is read and written with volatile accesses. | [22.2](./ch22-02-kernel-code) |
| registry | The default index of published packages, where `ceesafec fetch` looks up a dependency that names only a version requirement. | [7.4](./ch07-04-dependencies), [14.2](./ch14-02-publishing) |
| `replace` (the built-in) | A function that every file can call, `replace(mut place, value)`: it puts `value` in the place and returns the value that was there. Nothing is left without a value, so it takes a field out through an exclusive borrow, where a plain move is an error (N0251), also from a struct with a `drop` block. | [`type-system.md` §8](https://ceesafe.github.io/CeeSafe/reference/type-system) |
| `@repr(C)` | An attribute that makes a struct compatible with C: C code sees a `struct` of the same name and fields, so a value of it can cross to C. | [20.2](./ch20-02-calling-c) |
| `Result` | `Result<T, E>`: the type of a function's answer when it can fail. Its value is `Ok(v)` or `Err(e)`. | [9.2](./ch09-02-result) |
| return elision | See *elision rule*. | [10.3](./ch10-03-regions) |
| return type | The type after `->` in a function's declaration. A function without one returns nothing. | [3.3](./ch03-03-functions) |
| run-once environment | The environment of a closure whose body runs at most once: the `move` literal of `Thread.spawn`, and an `async move` child. Its body may move a capture out, once (N0250 for a second move). Any other `move` literal may be called again, so moving a capture out of it is an error (N0251). | [16.1](./ch16-01-starting-threads) |
| runtime | The C code that the compiler adds to every program it builds: allocation, panics, threads and the executor. A host starts it with `ceesafe_init`. | [14.3](./ch14-03-libraries-for-other-programs) |

## S

| Term | Meaning | Section |
|---|---|---|
| `Safety:` comment | A documentation comment with a line that starts `Safety:`. The compiler requires one on every `unsafe fn`, where it says what the caller must guarantee, and on every function that contains an `unsafe` block, where it says why the unsafe code is sound. The compiler does not check that it is true. | [20.1](./ch20-01-unsafe-code) |
| scope (of a name) | The part of the program where a name can be used: from a variable's declaration to the end of its block. All the files of a package share one scope for items. | [3.1](./ch03-01-variables), [7.2](./ch07-02-files-and-public) |
| scope (of tasks) | `Async.scope`: a call that starts several tasks, its children, and returns only when every one of them has returned. The first child that fails cancels the others. | [17.2](./ch17-02-scopes) |
| scoped type | The specification's and the diagnostics' word for a type whose values may hold a borrow: a borrow, a slice, a `StringView`, a closure, a struct or an enum with a region parameter, and an optional, a `Result`, a collection or a generic type with a scoped part. In generic code a value of a type parameter's type is treated as scoped, unless the parameter is bounded by `Send` or `Share`. | [10.3](./ch10-03-regions), [10.2](./ch10-02-interfaces) |
| scratch frame | Short-lived memory opened with `Scratch.enter()` and bound with `using`. Allocations come from the frame, and the end of its block releases them all. | [15.3](./ch15-03-allocators) |
| `Send` | Said of a type whose values may be moved to another thread. The compiler works it out from the type's parts, and a program cannot declare it. No type that can hold a borrow or a raw pointer is `Send`, so a bound `T: Send` also says that a `T` holds no borrow. | [16.4](./ch16-04-send-and-share), [10.2](./ch10-02-interfaces) |
| shadowing | Declaring a name again while it is in scope. CeeSafe has none: it is an error. | [3.1](./ch03-01-variables) |
| `Share` | Said of a type of which several threads may use one value at the same time, through shared borrows: a `Send` type that owns nothing that changes through a shared borrow, such as a cell. A mutex and an atomic are `Share`. | [16.4](./ch16-04-send-and-share) |
| shared borrow | A `borrow T`. There can be many at once, and while one is live the value cannot be changed, moved or destroyed. Only the contents of a *cell* change through one. | [4.2](./ch04-02-borrowing) |
| shared ownership | One value with several owners, which is destroyed when the last owner is. `Arc<T>` is the one type that has it: `T` must be `Share`, and an owner gets only a shared borrow of the value. | [15.5](./ch15-05-shared-ownership) |
| shared reference | The specification's and the compiler's word for a shared borrow. See *shared borrow*. | [4.2](./ch04-02-borrowing) |
| shield | `Async.shield(ms, body)`: an async closure in which waits ignore a cancellation, for at most `ms` milliseconds. It is for cleanup that has to wait. | [17.3](./ch17-03-timeouts-races-cancellation) |
| slice | A borrow of consecutive elements of an array or a vector: `Slice<T>` is shared and `SliceMut<T>` is exclusive. | [4.3](./ch04-03-slices-and-views) |
| smart pointer | See *box* and *shared ownership*. | [15](./ch15-00-memory) |
| spawn | To start a thread or a child task. `Thread.spawn(move () => …)` starts a thread and returns its *handle*, `s.spawn(async () => …)` starts a child of a scope, and `set.spawn(async move () => …)` a child of a task set. | [16.1](./ch16-01-starting-threads), [17.2](./ch17-02-scopes) |
| stack | The memory of the running functions, where local variables live. A function's part is given back when it returns. | [4.1](./ch04-01-what-ownership-is) |
| standard error | The stream a program writes messages about the run to: errors, warnings, usage. `Console.writeErrorLine` writes to it. | [12.6](./ch12-06-standard-error) |
| standard output | The stream a program writes its results to. `Console.writeLine` writes to it. | [12.6](./ch12-06-standard-error) |
| statement | One step of a function's body, ended by `;` or by a block: a declaration, an assignment, a call, an `if`, a loop. | [3.3](./ch03-03-functions) |
| static (an item) | One value for the whole run, at a fixed address, declared at the top level with `static`. Its type must be `Share`, or static text or read-only C data, and it is never destroyed. Every use of a `static mut` is allowed only in an *unsafe context*. | [22.2](./ch22-02-kernel-code) |
| static function | A function in a struct or an enum with no `this`. It is called on the type, `Order.create(…)`, and is how a value is made. | [5.3](./ch05-03-methods) |
| `static` (the region) | The region of data that lives as long as the program runs, such as a string literal. | [10.3](./ch10-03-regions) |
| stream | One network connection, read and written as bytes: a `TcpStream`. See also *standard output* and *standard error*. | [21.1](./ch21-01-one-connection) |
| string | A `String`: text that the value owns, kept on the heap as UTF-8, and able to grow. | [8.2](./ch08-02-strings) |
| string view | A `StringView`: a shared borrow of text. A string literal is one, and a `String` lends one. | [4.3](./ch04-03-slices-and-views), [8.2](./ch08-02-strings) |
| struct | A type that groups named values, its fields, and can have methods. | [5.1](./ch05-01-defining-structs) |
| struct literal | The expression that makes a value of a struct, with every field named: `Player { score: 0, lives: 3 }`. | [5.1](./ch05-01-defining-structs) |
| structured concurrency | The rule that every task has an owner that waits for it: a task is awaited where it is called, or it is a *child*, and what started a child returns only after the child has. No task runs loose in the background. | [17](./ch17-00-async), [17.2](./ch17-02-scopes) |
| supertrait | An interface that another interface requires: in `interface Shape: Named`, `Named` is a supertrait of `Shape`. It is the one place where CeeSafe uses the word "trait". | [20.3](./ch20-03-more-interfaces-and-generics) |
| suspend | Said of a task that gives up its thread at an `await` until what it waits for is ready. | [17.1](./ch17-01-tasks-and-await) |
| suspension point | A place in an async function where its task may stop and let another task use the thread: an `await`. The code between two suspension points runs without interruption, and a local that is used after one is kept in the task's frame. | [17.1](./ch17-01-tasks-and-await) |
| `swap` (the built-in) | A function that every file can call, `swap(mut a, mut b)`: it exchanges the values of two places of one type, and leaves neither without a value. `v.swap(i, j)`, which exchanges two elements, and an atomic's `swap` are methods of the library. | [`type-system.md` §8](https://ceesafe.github.io/CeeSafe/reference/type-system) |
| sweep | A run of `ceesafec test` that runs each passing test again and again with one failure put in at each possible place: `--sweep alloc` for allocations, `--sweep cancel` for cancellations. | [11.3](./ch11-03-organising-tests) |

## T

| Term | Meaning | Section |
|---|---|---|
| table (of a `dyn` value) | The constant data a `dyn` value points to beside its value: how to destroy the value, its size, and one function for each method of the interface. | [18.1](./ch18-01-dynamic-dispatch) |
| task | A call of an async function or of an async closure: a function call that can stop where it has to wait and let another task use the thread. It is not a value: it runs only when it is awaited, given to `Async.run` or spawned, where the call is written. | [17.1](./ch17-01-tasks-and-await) |
| task set | `Async.tasks`: a call that runs a body with a set, to which the body and the functions it calls add children while it runs, such as one task for each connection that arrives. A child owns what it captures, a child's error does not cancel the others, and the call returns when the body and every child have returned. | [17.2](./ch17-02-scopes) |
| temporary | A value that is made in an expression and not given a name. It is destroyed at the end of its statement. | [4.3](./ch04-03-slices-and-views) |
| test | A function marked `@test`, which runs part of the program and checks the answer. `ceesafec test` runs each one as its own process. | [11.1](./ch11-01-writing-tests) |
| thread | A part of a program that the operating system runs at the same time as its other threads. `Thread.spawn` starts one. | [16.1](./ch16-01-starting-threads) |
| timeout | `Async.timeout(ms, error, body)`: a call that runs an async closure beside a deadline `ms` milliseconds away. If the deadline passes first, the body is cancelled and awaited, and an error from it becomes `error`. | [17.3](./ch17-03-timeouts-races-cancellation) |
| trait | CeeSafe's term is *interface*. | [10.2](./ch10-02-interfaces) |
| tuple | CeeSafe has none. Use a *struct* with named fields. | [3.2](./ch03-02-data-types) |
| two-phase borrow | The rule that lets an argument read the value that its call is about to change. In `v.push(v.length)`, and in `add(mut v, v.length)`, the exclusive borrow of `v` starts only when the arguments have been evaluated, so they may read `v`. They may not keep a borrow of it: `v.extend(v.view())` is an error (N0241). | [`ownership.md` B5](https://ceesafe.github.io/CeeSafe/reference/ownership) |
| type alias | CeeSafe has none. Write the type, or make a struct. | [3.2](./ch03-02-data-types) |
| type argument | The type that a use of a generic puts in place of a type parameter: the `u8` in `Vector<u8>`. A call usually infers it. | [10.1](./ch10-01-generic-types) |
| type parameter | A name that stands for a type in a generic function or type, written between angle brackets after its name. Generic code treats a value of its type as one that may hold a borrow, unless a bound `T: Send` or `T: Share` rules that out. | [10.1](./ch10-01-generic-types), [10.2](./ch10-02-interfaces) |

## U

| Term | Meaning | Section |
|---|---|---|
| unreachable | Said of an arm when every value it would take is taken by the arms above it. | [19.2](./ch19-02-patterns-that-can-fail) |
| unsafe context | The inside of `unsafe { … }` or the body of an `unsafe fn`. Only there may code read or write through a raw pointer, do arithmetic on one, call C or an `unsafe fn`, or use a `static mut`. It relaxes no other check. | [20.1](./ch20-01-unsafe-code) |
| `unsafe fn` | A function declared `unsafe fn`. Its body is an *unsafe context*, and a call of it is allowed only in one, because the caller must keep what its *`Safety:` comment* asks. | [20.1](./ch20-01-unsafe-code) |
| unwrap | To take the value out of an optional or a `Result` with `!`. The program panics if there is none. | [6.3](./ch06-03-optionals), [9.2](./ch09-02-result) |
| `using` | A declaration, `using x = e;`, of a variable that cannot be moved or assigned as a whole, so that its value is destroyed exactly at the end of its block: a guard, an arena, a scratch frame. | [15.2](./ch15-02-destruction) |
| UTF-8 | The encoding of all text in CeeSafe. A `String` and a `StringView` are bytes that are valid UTF-8, and their lengths count bytes. | [8.2](./ch08-02-strings) |

## V

| Term | Meaning | Section |
|---|---|---|
| variable | A name for a value, declared with `let`, which cannot be assigned again, or with `var`, which can. | [3.1](./ch03-01-variables) |
| variant | One of the alternatives of an enum, written `Enum.Variant`. It can carry a payload. | [6.1](./ch06-01-defining-an-enum) |
| vector | A `Vector<T>`: a list of values of one type that lives on the heap and grows at its end. | [8.1](./ch08-01-vectors) |
| version requirement | The versions of a dependency that a package accepts, written in its manifest, such as `tally = "0.1"`. | [7.4](./ch07-04-dependencies) |
| view | A shared borrow of text or of elements that copies nothing: a `StringView`, or the slice that `v.view()` gives of a vector. | [4.3](./ch04-03-slices-and-views) |
| volatile | Said of an access to memory that the compiler must perform exactly as written, once and in order, as a device's registers need. | [22.2](./ch22-02-kernel-code) |
| vtable | CeeSafe's term is *table*. | [18.1](./ch18-01-dynamic-dispatch) |

## W

| Term | Meaning | Section |
|---|---|---|
| warning | A diagnostic that does not stop the build. `--deny-warnings` makes each one an error. | [9.2](./ch09-02-result), [14.1](./ch14-01-build-settings) |
| work stealing | A worker thread with nothing to do taking a child task that another worker has not started yet. | [17.5](./ch17-05-tasks-and-threads) |
| worker | A thread that takes work from others: a thread of a pool, a thread that `Async.blocking` runs a closure on, or one of the threads of `Async.runWorkers`. | [17.5](./ch17-05-tasks-and-threads), [21.3](./ch21-03-thread-pool) |
