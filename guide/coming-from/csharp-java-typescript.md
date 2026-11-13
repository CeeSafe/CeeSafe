# CeeSafe for C#, Java and TypeScript programmers

CeeSafe's syntax was chosen to look like C# and TypeScript: braces, `fn name(x: T) -> R`, `let` and `var`, generics in
angle brackets, `public` and `internal`, `using`, interfaces. Underneath, it works very differently: there is no
garbage collector, no classes, no inheritance, no exceptions and no null references. Memory is managed at compile
time, by tracking ownership.

## At a glance

| C# / Java / TypeScript | CeeSafe | Notes |
|---|---|---|
| `var x = 1;` / `const x = 1;` | `var x = 1;` / `let x = 1;` | |
| `class` | `struct` with methods | no inheritance; values, not references |
| `interface`, `class C : I` | `interface`, `impl I for C` | interfaces are generic bounds, not types |
| `new Point(1, 2)` | `Point { x: 1, y: 2 }` | on the stack, or `Box.create(…)` on the heap |
| garbage collection | ownership: destroyed when the owner goes away | deterministic, at a point the language defines |
| `IDisposable`, `using` | `drop { … }`, `using` | every owner runs `drop`, not only `using` |
| `null` references | `T?`, which must be checked | a plain `T` is never null |
| exceptions, `try`/`catch` | `Result<T, E>`, `?`, `match` | panics are bugs and cannot be caught |
| `ref`, `out` | `borrow mut T` parameters, `mut x` at the call | |
| `List<T>`, `string` | `Vector<T>`, `String` / `StringView` | |
| LINQ, lambdas | `sortBy`, `forEach`, `any`, `all`, `retain`; `(x) => x > 2` | lambdas borrow and cannot escape |
| `async` / `await`, `Task.WhenAll`, `Task.WhenAny` | `async fn`, `await`; `Async.scope`, `Async.all`, `Async.race` | a task is awaited where it starts, never stored; see [Async tasks](../async.md) |
| threads | `Thread.spawn`, `Thread.both`, `Arc`, `Mutex`, atomics | see [Threads](../threads.md) |
| NuGet, Maven, npm | `ceesafe.toml`, path and git dependencies | |

## The same program

```csharp
class Account {
    public long Balance { get; private set; }
    public void Deposit(long amount) => Balance += amount;
}

var a = new Account();
a.Deposit(100);
Console.WriteLine(a.Balance);
```

```ceesafe
struct Account {
    private balance: i64;

    public fn create() -> Account => Account { balance: 0 };
    public fn deposit(borrow mut this, amount: i64) {
        this.balance += amount;
    }
    public fn current(borrow this) -> i64 => this.balance;     // a method cannot share a field's name
}

fn main() {
    var a = Account.create();
    a.deposit(100);
    Console.writeLine(a.current());
}
```

```text
100
```

The receiver says how a method uses its object: `borrow this` reads it, `borrow mut this` changes it, and `this`
takes it by value. Calling `deposit` needs `a` to be a `var`.

## What changes

**Values, not references.** Assigning a struct copies it (if every field is copyable) or moves it, never shares it.
Two variables never point to the same object unless one *borrows* the other, and a borrow is checked: while something
is borrowed exclusively, nothing else can use it.

**Destruction is deterministic.** An object is destroyed when its owner goes out of scope, by assignment, or when the
object that owns it is destroyed. A `drop` block runs then, exactly once: closing a file or releasing a lock does not
depend on a collector or a `finally`.

**Errors are return values.** A function that can fail returns `Result<T, E>`; the caller matches it, or passes it on
with `?`. Out of memory is an error value too. Bugs (an index out of range, an overflow) stop the program instead of
throwing.

**No null.** `StringView` can never be null; `StringView?` can, and the compiler requires a check before use.

**Generics are compiled per type,** like C++ templates and unlike Java's erasure, and checked once, like C#'s.
Interfaces constrain type parameters (`<T: Shape>`), but there is no `List<Shape>` holding different shapes: use an
enum.

## Things you will miss

Classes and inheritance, interfaces as types, reflection, attributes on everything, properties, events, LINQ's query
syntax, string interpolation, exceptions, a task you can store and await later, and a large standard library.
