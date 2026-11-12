---
title: Strings and string views
summary: String owns UTF-8 text and StringView borrows it - building text with append and the number helpers, copying it with String.from and clone, length in bytes, iterating bytes, slicing on character boundaries, comparing, searching, splitting, and converting numbers to and from text.
assumes: [ch04-02-borrowing, ch04-03-slices-and-views, ch06-03-optionals, ch08-01-vectors]
keywords: [String, StringView, UTF-8, String.from, clone, append, appendNumber, appendFixed, appendHex, appendPadded, bytes, split, splitOnce, trim, parseInt, parseFloat, string interpolation]
diagnostics: [N0205, N0207, N0400]
specification: ["type-system.md §23.2"]
---

# Strings and string views

**This section teaches** the two types that hold text: `String`, which owns it, and `StringView`, which borrows
it. It assumes [borrowing](./ch04-02-borrowing), [optionals](./ch06-03-optionals) and
[vectors](./ch08-01-vectors), because a `String` behaves like a vector of bytes.

## Two types

- A **`StringView`** is a borrow of text that lives somewhere else. It cannot change the text. A string literal,
  `"hello"`, is a `StringView`: the text is in the program itself.
- A **`String`** owns text on the heap. It can grow. Like a vector, it frees its memory when it goes away.

<span class="listing-caption"><strong>Listing 8-15</strong>, <code>main.cee</code>: A <code>String</code> is built from views, and lends its text as a view.</span>

```ceesafe
fn shout(text: StringView) {
    Console.write(text);
    Console.writeLine("!");
}

fn main() {
    let greeting: StringView = "Hello";
    var message = String.create();
    message.append(greeting)!;
    message.append(", Ada")!;
    shout(message);
    shout("Goodbye");
    let view = message.view();
    Console.writeLine(view.length);
}
```

```text
Hello, Ada!
Goodbye!
10
```

- `String.create()` makes an empty string and allocates nothing. `String.withCapacity(n)!` makes one with room
  for `n` bytes, and `String.from(text)!` makes one that holds a copy of a text ([below](#copies-of-text)).
- `append` adds the text of a view to the end. It may allocate, so it returns a `Result`, as `push` does.
- `shout` takes a `StringView`, so it accepts a literal and a `String` alike: `shout(message)` borrows the
  string's text.
- `message.view()` gives that borrow as a value.

::: tip Rule
A function that reads text takes a `StringView`. A function that makes text returns a `String`. A struct that
keeps text holds a `String`.
:::

A view of a `String` borrows the string. As with [a vector's elements](./ch08-01-vectors), the string cannot
grow, be cleared or go away while the view is live.

## Text is UTF-8

Text is stored as UTF-8: one byte for an ASCII character, and two to four bytes for any other. There is no
character type. A character literal such as `'k'` is an integer, the character's Unicode number.

A `StringView` is always valid UTF-8, and a `String` only receives text from views, so it is always valid too.

## Building text

<span class="listing-caption"><strong>Listing 8-16</strong>, <code>main.cee</code>: Text and numbers are appended to a <code>String</code>, each with its own call.</span>

```ceesafe
fn main() {
    var line = String.create();
    line.append("item ")!;
    line.appendNumber(-17)!;
    line.append(" costs ")!;
    line.appendFixed(3.14159, 2)!;
    line.append(", code ")!;
    line.appendHex(255, 4, false)!;
    line.append(" [")!;
    line.appendPadded("id", 6, TextAlign.Right, ' ')!;
    line.append("]")!;
    Console.writeLine(line);
}
```

```text
item -17 costs 3.14, code 00ff [    id]
```

| Method | What it appends |
|---|---|
| `s.append(view)` | the text of a view |
| `s.appendNumber(n)` | an integer in decimal |
| `s.appendFixed(f, digits)` | an `f64` with exactly `digits` digits after the point |
| `s.appendHex(n, minWidth, upper)` | a `u64` in hexadecimal, with zeros up to `minWidth` digits, in capitals if `upper` |
| `s.appendOctal(n)` | a `u64` in octal |
| `s.appendPadded(view, width, align, fill)` | a view in a field of `width` characters; `align` is `TextAlign.Left`, `Right` or `Center` |

Each returns a `Result`. `s.clear()` empties the string and keeps its capacity.

::: tip Rule
There is no string interpolation and no format string. Text is built with one call for each piece.
:::

## Copies of text

When the text is already there, as a literal, a view or another `String`, one call makes a `String` of it.

<span class="listing-caption"><strong>Listing 8-17</strong>, <code>main.cee</code>: A <code>String</code> from a literal, a clone of it, and a <code>String</code> from part of another.</span>

```ceesafe
fn main() {
    let name = String.from("Ada")!;
    var longer = name.clone()!;
    longer.append(" Lovelace")!;
    let last = String.from(longer[4..])!;
    Console.writeLine(name);
    Console.writeLine(longer);
    Console.writeLine(last);
}
```

```text
Ada
Ada Lovelace
Lovelace
```

- `String.from(text)` takes a `StringView` and returns a new `String` holding a copy of it. A literal, a `String`
  (which lends its view) and a slice of either are all accepted.
- `s.clone()` is a second `String` with the same text: `String.from(s)`, written as a method. It reads `s` and
  does not move it.
- Each copy has its own memory. Appending to `longer` does not change `name`.
- Both allocate once, for exactly the bytes they copy, and so both return a `Result`. An empty text allocates
  nothing.

Use `String.from` when the text is known at once, and `String.create()` with `append` when it is put together
from pieces. A `StringView` has no `clone`: copying a view is an assignment, and it is still a borrow of the same
text.

## Length and bytes

<span class="listing-caption"><strong>Listing 8-18</strong>, <code>main.cee</code>: The length counts bytes. The loop counts characters.</span>

```ceesafe
fn main() {
    let word: StringView = "café";
    Console.writeLine(word.length);
    Console.writeLine(word[0]);

    var characters = 0;
    for (b in word.bytes()) {
        if ((b & 0xC0) != 0x80) {
            characters += 1;
        }
    }
    Console.writeLine(characters);
}
```

```text
5
99
4
```

- `word.length` is 5, not 4: `é` takes two bytes.
- `word[0]` is one byte, a `u8`: 99 is the letter `c`.
- `word.bytes()` is the text as a `Slice<u8>`, which a `for` loop visits byte by byte.

The library has no iterator over characters. In UTF-8, every byte of a character after its first has the form
`10xxxxxx`, so the loop above counts the bytes that do not.

## Slicing a view

`s[lo..hi]` is a view of the bytes from `lo` up to `hi`. `s[lo..]` and `s[..hi]` leave an end out.

<span class="listing-caption"><strong>Listing 8-19</strong>, <code>main.cee</code>: Three views of parts of one text.</span>

```ceesafe
fn main() {
    let text: StringView = "café au lait";
    Console.writeLine(text[0..3]);
    Console.writeLine(text[0..5]);
    Console.writeLine(text[6..]);
}
```

```text
caf
café
au lait
```

The positions are byte positions. A slice must begin and end between characters, and the program checks it:

<span class="listing-caption"><strong>Listing 8-20</strong>, <code>main.cee</code>: Byte 4 is the middle of <code>é</code>. The program panics.</span>

<!-- ceesafe: panics -->
```ceesafe
fn main() {
    let text: StringView = "café au lait";
    Console.writeLine(text[0..4]);
}
```

The program stops with:

```text
ceesafe panic: string slice not on a character boundary at main.cee:3:23
```

::: tip Rule
A slice of text is checked twice: both ends are in range, and both ends fall between characters. So a view is
always valid UTF-8.

*Specification: `type-system.md` §23.2.*
:::

Take positions from a search (`indexOf`, below) and not from counting by hand: a position that a search returns
is always a boundary.

## Comparing and searching

<span class="listing-caption"><strong>Listing 8-21</strong>, <code>main.cee</code>: Text is compared with <code>==</code>, and searched with methods.</span>

```ceesafe
fn main() {
    let path: StringView = "notes/todo.txt";
    Console.writeLine(path == "notes/todo.txt");
    Console.writeLine(path.startsWith("notes/"));
    Console.writeLine(path.endsWith(".md"));
    Console.writeLine(path.contains("todo"));
    Console.writeLine("TXT".equalsIgnoreAsciiCase("txt"));
    match (path.indexOf("/")) {
        Some(at) => { Console.writeLine(path[at + 1..]); }
        null => { Console.writeLine("no directory"); }
    }
}
```

```text
true
true
false
true
true
todo.txt
```

`==` and `!=` compare the bytes of two views, or of two `String`s. To compare a `String` with a view, compare
its view: `name.view() == "Ada"`. `indexOf` and `lastIndexOf` return an optional position, `usize?`. Views have no
`<`: text is not ordered by an operator.

## Splitting

<span class="listing-caption"><strong>Listing 8-22</strong>, <code>main.cee</code>: A line is cut at a separator once, and a list is cut at every comma.</span>

```ceesafe
fn main() {
    let line = "  colours = red,green,,blue \n";
    match (line.trim().splitOnce("=")) {
        Some(pair) => {
            Console.writeLine(pair.left.trim());
            for (colour in pair.right.trim().split(",")) {
                Console.write("[");
                Console.write(colour);
                Console.writeLine("]");
            }
        }
        null => { Console.writeLine("no '='"); }
    }
}
```

```text
colours
[red]
[green]
[]
[blue]
```

- `trim()` is the view without the ASCII whitespace at its ends; `trimStart()` and `trimEnd()` do one end.
- `splitOnce(sep)` cuts at the first separator. It returns an optional with the two halves, `left` and `right`,
  or `null` if the separator is not there.
- `split(sep)` gives every piece in turn, to a `for` loop. Two separators in a row give an empty piece.

Every piece is a view of the same text. Nothing is copied or allocated.

## Numbers from text

<span class="listing-caption"><strong>Listing 8-23</strong>, <code>main.cee</code>: Parsing gives an optional, <code>null</code> when the text is not a number of that type.</span>

```ceesafe
fn main() {
    Console.writeLine("42".parseInt<i32>() ?? -1);
    Console.writeLine("4x".parseInt<i32>() ?? -1);
    Console.writeLine("300".parseInt<u8>() ?? 0);
    Console.writeLine("ff".parseIntRadix<u32>(16) ?? 0);
    Console.writeLine(" 2.5 ".trim().parseFloat<f64>() ?? 0.0);
}
```

```text
42
-1
0
255
2.5
```

`parseInt<T>()` names the integer type it parses to. `"300"` is not a `u8`, so the result is `null`. The parsers
do not skip spaces: trim first. The other direction, a number to text, is `appendNumber` and `appendFixed`.

## Mistakes

::: warning Mistake: a literal where a `String` is expected
A literal is a `StringView`. Nothing turns it into a `String` for you, because that allocates.
:::

<span class="listing-caption"><strong>Listing 8-24</strong>, <code>main.cee</code>: A view is given to a <code>String</code> variable. This does not compile.</span>

<!-- ceesafe: fail N0205 -->
```ceesafe
fn main() {
    var name: String = "Ada";
    Console.writeLine(name);
}
```

```text
main.cee:2:24: error[N0205]: expected 'String', found 'StringView'
  |
2 |     var name: String = "Ada";
  |                        ^^^^^
```

Ask for the copy: `var name = String.from("Ada")!;`.

::: warning Mistake: joining text with `+`
`+` does not join text at run time. The result would need memory, and an operator cannot report a failure.
:::

<span class="listing-caption"><strong>Listing 8-25</strong>, <code>main.cee</code>: Two views are added. This does not compile.</span>

<!-- ceesafe: fail N0207 -->
```ceesafe
fn main() {
    let first: StringView = "Ada";
    let full = first + " Lovelace";
    Console.writeLine(full);
}
```

```text
main.cee:3:16: error[N0207]: operator '+' cannot be applied to type 'StringView'; views compare with '==' and '!=', and only a constant or static initialiser joins them with '+' (append to a 'String' at run time)
  |
3 |     let full = first + " Lovelace";
  |                ^^^^^^^^^^^^^^^^^^^
```

The compiler joins literals with `+` only in a constant. Append both to a `String`, as Listing 8-15 does.

::: warning Mistake: braces in a string
`{count}` inside a string is those seven characters. The compiler warns, and the program runs.
:::

<span class="listing-caption"><strong>Listing 8-26</strong>, <code>main.cee</code>: The braces are appended as written. The compiler warns.</span>

<!-- ceesafe: warns N0400 -->
```ceesafe
fn main() {
    let count = 3;
    var line = String.create();
    line.append("count = {count}")!;
    Console.writeLine(line);
}
```

```text
main.cee:4:26: warning[N0400]: this string prints '{count}' as written (there is no string interpolation): write the value in a call of its own, or '\{' to mean the brace
  |
4 |     line.append("count = {count}")!;
  |                          ^^^^^^^
```

Write `line.append("count = ")!;` and then `line.appendNumber(count)!;`.

::: details Coming from Rust
`String` is `String` and `StringView` is `&str`. A `String` argument is lent to a `StringView` parameter without
`&` or `.as_str()`. `String.from("…")` is `String::from` and `s.clone()` is `clone`, but each returns a `Result`,
as `append` does. There is no `format!`, no `to_string()` and no `chars()`. `s[lo..hi]` panics off a character
boundary, as in Rust.
:::

::: details Coming from C#, Java, TypeScript, Go or Python
A string here is not an immutable object that is shared freely. A `String` has one owner and can change; a
`StringView` is a short borrow of it. `length` counts UTF-8 bytes, not UTF-16 units or characters. There is no
`+`, no `$"{x}"`, no template literal and no f-string.
:::

::: details Coming from C
A `StringView` is a pointer and a length, with no NUL at the end, and it is always valid UTF-8. A `String` is a
buffer that knows its length and capacity, and `append` cannot write past it.
:::

## Summary

- `String` owns text on the heap; `StringView` borrows text. A literal is a `StringView` (`N0205`).
- Take a `StringView` as a parameter; a `String` argument is lent to it.
- Text is UTF-8. `length` is in bytes, `s[i]` is a byte, and `s.bytes()` is a `Slice<u8>`.
- `String.from(text)!` makes a `String` from a view, and `s.clone()!` makes a second `String` from a first.
- Build text with `append`, `appendNumber`, `appendFixed`, `appendHex` and `appendPadded`. There is no `+` on
  text (`N0207`) and no interpolation (`N0400`).
- `s[lo..hi]` is a view; it panics if an end is inside a character.
- `==` compares bytes. `contains`, `startsWith`, `endsWith` and `indexOf` search.
- `trim`, `splitOnce` and `split` give views of parts of the text.
- `parseInt<T>()`, `parseIntRadix<T>(radix)` and `parseFloat<T>()` return an optional.

Next: [hash maps and ordered maps](./ch08-03-maps), which often have a `String` as their key.
