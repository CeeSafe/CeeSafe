---
title: Where patterns go
summary: The three places a pattern is written - the arms of a match, after the operator is, and a let with an else block - where the names an is pattern binds can be used, and what to write in a plain let, for and parameters, which take only a name.
assumes: [ch06-02-match, ch06-03-optionals]
keywords: [pattern, match, is, binding, "if let", "while let", "let else", N0359]
diagnostics: [N0100, N0200, N0359]
specification: ["type-system.md §15.2", "type-system.md §15.3", "type-system.md §15.4", "type-system.md §15.5", "grammar.md §2"]
---

# Where patterns go

**This section teaches** the places in a program where a pattern may be written, and what to write in the places
where it may not. It assumes [`match`](./ch06-02-match) and [Optionals](./ch06-03-optionals).

A pattern describes a shape a value may have, such as `Some(n)` or `0..=9`. A value **fits** a pattern or does not,
and when it fits, the names in the pattern **bind** parts of it. CeeSafe has three places for a pattern:

- an arm of a `match`;
- the right side of the operator `is`;
- a `let` with an `else` block, `let PATTERN = e else { … };`.

Everything else that introduces a name (a plain `let`, `var`, `for`, a function's or a closure's parameters) takes
a name and no pattern.

## In the arms of a `match`

A `match` is a list of arms, and each arm starts with a pattern. The arms are tried from the top, and the first
whose pattern fits runs. A `match` has two forms, a statement and an expression, and their patterns are the same.

<span class="listing-caption"><strong>Listing 19-1</strong>, <code>main.cee</code>: A <code>match</code> statement and a <code>match</code> expression. Both take any pattern, and a guard after it.</span>

```ceesafe
enum Token {
    Number(i64),
    Symbol(u8),
    End,
}

fn show(token: Token) {
    match (token) {
        Token.Number(n) => {
            Console.write("number ");
            Console.writeLine(n);
        }
        Token.Symbol('+' | '-') => { Console.writeLine("a sign"); }
        Token.Symbol(_) => { Console.writeLine("another symbol"); }
        Token.End => { Console.writeLine("end"); }
    }
}

fn width(token: Token) -> usize => match (token) {
    Token.Number(n) if (n < 10) => 1,
    Token.Number(_) => 8,
    Token.Symbol(_) => 1,
    Token.End => 0,
};

fn main() {
    show(Token.Number(42));
    show(Token.Symbol('-'));
    show(Token.End);
    Console.writeLine(width(Token.Number(42)));
}
```

```text
number 42
a sign
end
8
```

- In `show`, the `match` begins a statement, so each arm is a pattern, `=>` and a block.
- In `width`, the `match` is a value, so each arm is a pattern, `=>` and an expression, with commas between arms.
- `if (n < 10)` after a pattern is a **guard**: the arm is taken only when the pattern fits and the guard is true.
  A guard is part of the arm, not of the pattern, so it is written in a `match` only.

The names an arm's pattern binds exist in that arm: in its guard and in its block or expression.

## After `is`

`value is pattern` is an expression of type `bool`: `true` when the value fits. It may stand wherever a `bool`
may.

<span class="listing-caption"><strong>Listing 19-2</strong>, <code>main.cee</code>: <code>is</code> gives a <code>bool</code>. A name its pattern binds can be used after <code>&amp;&amp;</code>.</span>

```ceesafe
enum Token {
    Number(i64),
    Symbol(u8),
    End,
}

fn main() {
    let c: u8 = '7';
    let isDigit = c is '0'..='9';
    Console.writeLine(isDigit);

    let token = Token.Number(12);
    Console.writeLine(token is Token.End);
    Console.writeLine(token is Token.Number(_) | Token.Symbol(_));

    let small = token is Token.Number(n) && n < 100;
    Console.writeLine(small);
}
```

```text
true
false
true
true
```

After `is`, the pattern runs as far as it can. So the `|` in `token is Token.Number(_) | Token.Symbol(_)` joins two
patterns: the test is true when the token fits either. `&&` and `||` are not part of a pattern, and end it.

::: tip Rule
`e is P` means what `match (e) { P => true, _ => false }` means. `e` is evaluated once, and `P` is any pattern a
`match` on `e` could have in an arm.

*Specification: `type-system.md` §15.4.*
:::

## Where an `is` pattern's names can be used

A name bound by a pattern has a value only if the value fitted. So the compiler lets you use the name only where
the test is known to have been true.

<span class="listing-caption"><strong>Listing 19-3</strong>, <code>main.cee</code>: The names of an <code>is</code> pattern reach the rest of an <code>&amp;&amp;</code> chain, the block of an <code>if</code>, the body of a <code>while</code> and the first branch of an <code>if</code> expression.</span>

```ceesafe
fn main() {
    let line = "name=Ada";
    if (line.indexOf("=") is Some(at) && at > 0) {
        Console.write("the key has ");
        Console.write(at);
        Console.writeLine(" bytes");
    } else {
        Console.writeLine("no key");
    }

    var stack = Vector<i32>.create();
    stack.push(3)!;
    stack.push(40)!;
    while (stack.pop() is Some(n)) {
        Console.writeLine(n);
    }

    let first: i32? = null;
    let shown = if (first is Some(n)) n else -1;
    Console.writeLine(shown);
}
```

```text
the key has 4 bytes
40
3
-1
```

- `indexOf` returns a `usize?`. `at` is bound when it is `Some`, and `&& at > 0` and the `if`'s block use it. The
  `else` block cannot: there, the test may have failed.
- `stack.pop()` returns an `i32?` ([Vectors](./ch08-01-vectors)). The `while` calls it again on each pass, and ends
  at the first `null`.
- The two `n` do not collide: each ends with its loop body or its branch.

::: tip Rule
The names an `is` pattern binds are in scope in the later operands of the same `&&` chain, and, when that chain is
the condition of an `if` or a `while`, in the `if`'s first block or branch or the `while`'s body. A pattern that
binds a name anywhere else is refused (`N0359`).

*Specification: `type-system.md` §15.4.*
:::

A pattern that binds nothing may be used anywhere, also under `!` and `||`: `!(token is Token.End)` is fine.

## Where only a name goes, and `let … else`

A plain `let` and a `var` take one name, or `_` to throw a value away (`let _ = f();`). `for (item in items)` takes
one name. A parameter is a name and a type. None of them takes a pattern, so the taking apart is done by a `match`
or an `is` next to them. A `let` takes a pattern when an `else` block follows the value, to say what happens when
the value does not fit.

<span class="listing-caption"><strong>Listing 19-4</strong>, <code>main.cee</code>: A loop over optionals with <code>is</code> in its body, and a <code>let … else</code> that takes a value out or leaves the function.</span>

```ceesafe
fn total(items: Slice<i32?>) -> i32 {
    var sum = 0;
    for (item in items) {
        if (item is Some(n)) {
            sum += n;
        }
    }
    return sum;
}

fn half(value: i32?) -> i32 {
    let Some(n) = value else {
        return -1;
    };
    return n / 2;
}

fn main() {
    let items: [i32?; 3] = [Some(1), null, Some(3)];
    Console.writeLine(total(items[..]));
    Console.writeLine(half(Some(10)));
    Console.writeLine(half(null));
}
```

```text
4
5
-1
```

`half` shows the way to write "take the value out, or leave". The value is tested against the pattern, as `is`
tests it. When it fits, the pattern's names are bound for the rest of the enclosing block: after the statement, `n`
is a plain `i32`. When it does not fit, the `else` block runs. That block cannot use the names, and it must leave:
with `return`, `break`, `continue`, or a call that does not return, such as a panic. A block whose end can be
reached is refused (N0213). The statement ends with `;`, as every `let` does.

::: tip Rule
`let P = e else { B };` evaluates `e` once. If the value fits `P`, the names of `P` are in scope to the end of the
enclosing block. If not, `B` runs, and `B` must leave.

*Specification: `type-system.md` §15.5.*
:::

## Mistakes

::: warning Mistake: keeping the `bool` of an `is` that binds
A test stored in a variable or passed as an argument has no block where its names would be used, so the compiler
refuses the names.
:::

<span class="listing-caption"><strong>Listing 19-5</strong>, <code>main.cee</code>: Nothing could use <code>v</code>. This does not compile.</span>

<!-- ceesafe: fail N0359 -->
```ceesafe
fn main() {
    let x: i32? = 5;
    let has = x is Some(v);
    Console.writeLine(has);
}
```

```text
main.cee:3:20: error[N0359]: this pattern binds 'v' where the test is not known true; an 'is' pattern binds only before a later '&&' operand, or in the condition of an 'if' or 'while' for its then-branch or body (ADR 0085 §2.8)
  |
3 |     let has = x is Some(v);
  |                    ^^^^^^^
```

Write `x is Some(_)`, which binds nothing, or `x.isSome()`.

::: warning Mistake: testing that a value does not fit, then using its names
`!(value is Some(n))` is true when the value did not fit, so there is no `n` after it.
:::

<span class="listing-caption"><strong>Listing 19-6</strong>, <code>main.cee</code>: An early return on the failing case, and a use of <code>n</code> after it. This does not compile.</span>

<!-- ceesafe: fail N0200 N0359 -->
```ceesafe
fn half(value: i32?) -> i32 {
    if (!(value is Some(n))) {
        return -1;
    }
    return n / 2;
}

fn main() {
    Console.writeLine(half(Some(10)));
}
```

```text
main.cee:2:20: error[N0359]: this pattern binds 'n' where the test is not known true; an 'is' pattern binds only before a later '&&' operand, or in the condition of an 'if' or 'while' for its then-branch or body (ADR 0085 §2.8)
  |
2 |     if (!(value is Some(n))) {
  |                    ^^^^^^^

main.cee:5:12: error[N0200]: cannot find 'n' in this scope
  |
5 |     return n / 2;
  |            ^
```

Write `let Some(n) = value else { return -1; };`, as `half` in Listing 19-4 does.

::: warning Mistake: a pattern in a `let` without `else`
A pattern that can fail needs an `else` block for the values that do not fit.
:::

<span class="listing-caption"><strong>Listing 19-7</strong>, <code>main.cee</code>: A <code>let</code> with a pattern and no <code>else</code>. This does not compile.</span>

<!-- ceesafe: fail N0100 -->
```ceesafe
fn main() {
    let x: i32? = 5;
    let Some(v) = x;
    Console.writeLine(v);
}
```

```text
main.cee:3:20: error[N0100]: expected 'else', found ';': 'let' with a pattern that can fail is 'let PATTERN = e else { … };', whose block leaves (return, break, continue, or a call that does not return) when the value does not match (ADR 0117); to test and go on, write 'if (e is PATTERN) { … }' or a 'match'
  |
3 |     let Some(v) = x;
  |                    ^
```

Write `let Some(v) = x else { return; };`, or `if (x is Some(v)) { … }`, or `let v = x ?? 0;` when a fallback will
do.

::: details Coming from Rust
`is` is one construct where Rust has four. `if let P = e` is `if (e is P)`. `while let P = e` is
`while (e is P)`. `matches!(e, P)` is `e is P`. A let chain is `e is P && …`. `let P = e else { … };` is Rust's
`let … else`. There are no tuples, so there is nothing for `let (a, b) = …` to take apart, and a plain
`let`, `for` and parameters take a name only. A struct's fields are read with `.`, not taken apart.
:::

::: details Coming from C#
`is` looks the same and binds differently. In C#, `if (!(o is int n)) return;` leaves `n` usable after the `if`.
In CeeSafe a name bound by `is` never leaves the `if`'s first block: write `let P = e else { return; };`. There is
no `is not`: write `!(e is P)` with a pattern that binds nothing. A `switch` expression is a `match` expression,
and `case P when cond` is the arm `P if (cond) =>`.
:::

## Summary

- A pattern is written in an arm of a `match` (statement or expression), after `is`, and in a `let … else`.
  Nowhere else.
- `e is P` is a `bool`, the test of a `match` with the one arm `P`. After `is` the pattern runs as far as it can,
  so `|` there joins patterns.
- The names an `is` pattern binds are usable after `&&` in the same chain, and in the block of the `if` or the
  body of the `while` whose condition the chain is. Elsewhere a binding pattern is `N0359`.
- A guard, `if (cond)`, follows a pattern in a `match` arm only.
- A plain `let`, `var`, `for` and parameters take a name. To take a value out or leave, write
  `let P = e else { … };`, whose block must leave.

Next: [Patterns that can fail](./ch19-02-patterns-that-can-fail), and how the compiler checks that a `match`
misses nothing.
