#!/usr/bin/env python3
"""Writes book/src/appendix-c-built-in-types.md: the page is this script's output, so a change to it is made here.

The page's text and its rows are below, as Python data (HEAD, the `table(…)` calls, REST_ROWS): they were written by
hand from the compiler's own lists (its N0200/N0234 messages, src/check/declare.cee's builtinName),
guide/builtins.md, spec/type-system.md and the API pages spec/api/core.md and spec/api/std.md. The script adds the
"Taught in" column from the chapters (book/src/ch*.md): the first of the table's home sections whose page names the
member (a row may name its sections itself), else a dash. So the page changes when a chapter starts or stops naming
a member.

usage: python3 tools/gen_appendix_c.py            (writes the page; review the diff)
       python3 tools/gen_appendix_c.py --check    (writes nothing; exit 1 when the committed page would change)
       python3 tools/gen_appendix_c.py --api      (records spec/api/'s signatures, once the rows agree with them)

A row's signature is the call as a program writes it (`s.trim() -> StringView`), not the library's declaration
(`fn trim(s: StringView<r>) -> StringView<r>`), so it is not copied from spec/api/. Instead
book/appendices/appendix-c.api.txt holds the declarations the rows were written from, and the script refuses to
write the page (and `--check` fails) while spec/api/ has another: look at the member's row, then run `--api`.

tools/book_check.py makes the same comparison, and compares the members with the compiler's lists
(tools/book_appendix_check.py).
"""
import glob
import os
import re
import sys

import book_appendix_gen

PAGE = "book/src/appendix-c-built-in-types.md"
SCRIPT = "tools/gen_appendix_c.py"
SOURCES = "its own tables and the chapters that teach each member (book/src/ch*.md)"

PAGES = {}  # the chapters' pages, {identifier: (text, its ceesafe blocks)}: `render` reads them


def load(root):
    PAGES.clear()
    for path in sorted(glob.glob(os.path.join(root, "book", "src", "ch*.md"))):
        ident = os.path.basename(path)[:-3]
        text = book_appendix_gen.read(root, f"book/src/{ident}.md")
        PAGES[ident] = (text, "\n".join(re.findall(r"```ceesafe\n(.*?)```", text, re.S)))
    book_appendix_gen.need(PAGES, "book/src/ has no chapter (ch*.md)")


def page(prefix):
    found = [p for p in PAGES if p.startswith(prefix)]
    book_appendix_gen.need(len(found) == 1, f"{SCRIPT} names the section '{prefix}', and book/src/ has {len(found)}"
                                            " such pages")
    return found[0]


def label(ident):
    chapter, section = int(ident[2:4]), int(ident[5:7])
    return f"[{chapter}.{section}](./{ident})" if section else f"[{chapter}](./{ident})"


def names_of(signature):
    """The member names a first cell documents: `x.name(`, `Type.name(`, `name<T>(`, `x.name` (a field)."""
    names = []
    for code in re.findall(r"`([^`]*)`", signature):
        m = re.match(r"([A-Za-z0-9<>, ]+\.)?([A-Za-z][A-Za-z0-9]*)(?:<[^>]*>)?(\(|$)", code)
        if m:
            names.append((m.group(2), m.group(3) == "(", m.group(1) is not None))
    return names


def taught(signature, homes, explicit):
    if explicit is not None:
        return ", ".join(label(page(p)) for p in explicit) if explicit else "–"
    names = names_of(signature)
    if not names:
        return "–"
    name, call, member = names[0]
    # In a home section the member may be named without its receiver (`clear()` in a sentence about a map).
    pattern = re.compile(r"(?<![A-Za-z0-9])" + name + (r"(<[^>\n`]*>)?\(" if call else r"\b"))
    if not call:
        pattern = re.compile(r"\." + name + r"\b")
    for home in homes:
        if pattern.search(PAGES[page(home)][0]):
            return label(page(home))
    return "–"


def table(title, intro, homes, rows, first="Member"):
    """A table of the page, as data: `written` gives its text once the chapters are read."""
    return (title, intro, homes, rows, first)


def written(title, intro, homes, rows, first):
    out = [f"## {title}", "", intro, "", f"| {first} | What it does | Fails or panics | Taught in |", "|---|---|---|---|"]
    for row in rows:
        signature, what, fails = row[0], row[1], row[2]
        explicit = row[3] if len(row) > 3 else None
        book_appendix_gen.need("|" not in signature + what + fails,
                               f"the row of {signature} ({SCRIPT}) has a `|`, which would end the table's cell")
        out.append(f"| {signature} | {what} | {fails} | {taught(signature, homes, explicit)} |")
    return "\n".join(out) + "\n"


NO = "No"
OOM = "`Err(AllocError.OutOfMemory)` when the allocation fails"
INDEX = "Panics `index out of bounds`"

TABLES = []

TABLES.append(table(
    "Integers",
    "The integer types are `i8`, `i16`, `i32`, `i64`, `isize`, `u8`, `u16`, `u32`, `u64` and `usize`. In the rows, "
    "`x` and `y` are two integers of one type `T`. The operators (`+`, `/`, `<<` and the others) are in "
    "[Appendix B](./appendix-b-operators): they panic on overflow, and these methods are how a program asks for something else.",
    ["ch03-02", "ch20-04"],
    [
        ("`x.wrappingAdd(y: T) -> T`, `x.wrappingSub(y: T) -> T`, `x.wrappingMul(y: T) -> T`",
         "The sum, difference or product of two integers, kept to the low bits of the type when it does not fit (two's complement for signed types).",
         NO),
        ("`x.checkedAdd(y: T) -> T?`, `x.checkedSub(y: T) -> T?`, `x.checkedMul(y: T) -> T?`",
         "The sum, difference or product of two integers as an optional.",
         "`null` when the result does not fit in the type"),
        ("`x.saturatingAdd(y: T) -> T`, `x.saturatingSub(y: T) -> T`, `x.saturatingMul(y: T) -> T`",
         "The sum, difference or product of two integers, or the type's least or greatest value when the result is beyond it.",
         NO),
        ("`x.mulHigh(y: T) -> T`",
         "The upper half of the full product of two integers; `wrappingMul` gives the lower half.",
         NO),
        ("`x.min(y: T) -> T`, `x.max(y: T) -> T`",
         "The smaller or the larger of two integers.",
         NO),
        ("`x.abs() -> T`",
         "The magnitude of a signed integer, in the same type. Unsigned types do not have it.",
         "Panics `integer overflow` for the type's least value"),
        ("`x.unsignedAbs() -> U`",
         "The magnitude of a signed integer, in the unsigned type `U` of the same width (`u32` for an `i32`).",
         NO),
        ("`x.rotateLeft(n: u32) -> T`, `x.rotateRight(n: u32) -> T`",
         "The bits of an unsigned integer rotated by `n` places, `n` taken modulo the type's width.",
         NO),
        ("`x.wrappingShl(n: u32) -> T`, `x.wrappingShr(n: u32) -> T`",
         "An unsigned integer shifted by `n` places, `n` taken modulo the type's width, where `<<` and `>>` panic for an `n` of the width or more.",
         NO),
        ("`x.leadingZeros() -> u32`, `x.trailingZeros() -> u32`, `x.countOnes() -> u32`",
         "The number of zero bits at the top or at the bottom of an unsigned integer (the width for 0), or the number of one bits.",
         NO),
        ("`x.byteSwap() -> T`",
         "An integer with its bytes in the opposite order.",
         NO),
    ],
))

TABLES.append(table(
    "Floats",
    "The float types are `f32` and `f64`. In the rows, `f` and `g` are two floats of one type `T`. None of these "
    "methods panics: a result that is not a number is the value NaN.",
    ["ch20-04", "ch03-02"],
    [
        ("`f.sqrt() -> T`", "The square root of a float, correctly rounded; NaN for a negative float.", NO),
        ("`f.abs() -> T`", "A float with its sign cleared.", NO),
        ("`f.floor() -> T`, `f.ceil() -> T`",
         "The nearest whole value below or above a float, still as a float.", NO),
        ("`f.min(g: T) -> T`, `f.max(g: T) -> T`",
         "The smaller or the larger of two floats; NaN if either is NaN.", NO),
        ("`f.bits() -> u32` (`f32`), `f.bits() -> u64` (`f64`)",
         "The IEEE 754 bit pattern of a float as an unsigned integer.", NO),
        ("`f32.fromBits(u: u32) -> f32`, `f64.fromBits(u: u64) -> f64`",
         "The float that has the bit pattern `u`. Every pattern is a value, and some are NaN.", NO),
    ],
))

TABLES.append(table(
    "Arrays",
    "`[T; N]` is `N` values of a Copy type `T`, side by side; `N` is at least 1 and part of the type. In the rows, "
    "`a` is an array.",
    ["ch03-02", "ch04-03"],
    [
        ("`[e1, e2, e3]`", "An array of the listed elements, which all have one type.", NO, ["ch03-02"]),
        ("`[v; N]`", "An array of `N` copies of `v`.", NO, ["ch03-02"]),
        ("`a.length`", "The number of elements of an array, a `usize` that is the `N` of its type.", NO),
        ("`a[i]`", "Element `i` of an array, counted from 0, to read or (in a `var`) to assign. `i` is a `usize`.",
         INDEX + " when `i` is not below the length", ["ch03-02"]),
        ("`a[lo..hi]`, `a[lo..]`, `a[..hi]`, `a[..]`",
         "A `Slice<T>` that borrows elements `lo` up to but not including `hi` of an array; a missing bound is the start or the end.",
         "Panics `slice range out of bounds` unless `lo <= hi <= a.length`", ["ch04-03"]),
        ("`borrow mut a[lo..hi]`",
         "A `SliceMut<T>` that borrows part of an array for writing. As an argument it is written `mut a[lo..hi]`.",
         "Panics `slice range out of bounds` unless `lo <= hi <= a.length`", ["ch04-03"]),
        ("`a == b`, `a != b`",
         "Whether two arrays of one type have equal elements. The elements are integers, `bool` or an enum without payloads.",
         NO, []),
        ("`for (x in a) { }`", "Runs the body once for each element of an array, in order.", NO, ["ch03-05"]),
        ("`a.fill(v)`, `a.sortBy(cmp)`, and every other method of a slice",
         "An array has the methods of its slice: `a.m()` is `a[..].m()`. The slice table lists them.",
         "As the slice's method", []),
    ],
    first="Form",
))

TABLES.append(table(
    "Slices: `Slice<T>` and `SliceMut<T>`",
    "A slice borrows a run of elements of an array, a vector or another slice: `Slice<T>` to read them, "
    "`SliceMut<T>` to write them. In the rows, `s` is either kind and `d` is a `SliceMut<T>`. A `Vector<T>` has every "
    "method of this table too, on its elements. "
    "The methods that take a closure are documented with "
    "[SliceAlgorithms](https://ceesafe.github.io/CeeSafe/reference/api/core#struct-slicealgorithms).",
    ["ch04-03", "ch08-01", "ch13-02", "ch16-01"],
    [
        ("`s.length`", "The number of elements a slice borrows, a `usize`.", NO),
        ("`s[i]`", "Element `i` of a slice, to read, or to assign through a `SliceMut<T>`.",
         INDEX + " when `i` is not below the length", ["ch04-03"]),
        ("`s[lo..hi]`, `s[lo..]`, `s[..hi]`, `s[..]`", "A slice of part of a slice.",
         "Panics `slice range out of bounds` unless `lo <= hi <= s.length`", ["ch04-03"]),
        ("`[]`", "The empty slice, where a `Slice<T>` or a `SliceMut<T>` is expected.", NO, []),
        ("`s == t`, `s != t`",
         "Whether two slices have the same length and equal elements. The elements are integers, `bool` or an enum without payloads.",
         NO, []),
        ("`for (x in s) { }`",
         "Runs the body once for each element of a slice: `x` is a copy of a Copy element, and a borrow of any other.",
         NO, ["ch04-03"]),
        ("`d.copyFrom(s: Slice<T>)`", "Copies every element of `s` into a mutable slice of the same length. `T` is Copy.",
         "Panics `slice lengths differ` when the lengths are not equal"),
        ("`d.fill(v: T)`", "Sets every element of a mutable slice to `v`. `T` is Copy.", NO),
        ("`d.swap(i: usize, j: usize)`", "Exchanges elements `i` and `j` of a mutable slice or a vector.",
         INDEX + " when `i` or `j` is not below the length"),
        ("`s.splitAt(mid: usize) -> SliceHalves<T>`",
         "The two parts of a slice as `left` (the elements before `mid`) and `right` (from `mid` on).",
         "Panics `slice range out of bounds` when `mid` is above the length"),
        ("`d.splitAtMut(mid: usize) -> SliceHalvesMut<T>`",
         "The two parts of a mutable slice as `left` and `right`, which may both be written, since they do not overlap.",
         "Panics `slice range out of bounds` when `mid` is above the length"),
        ("`d.sortBy(cmp: borrow mut fn(borrow T, borrow T) -> i32)`",
         "Sorts a mutable slice or a vector by a closure that returns a negative number, zero or a positive number for less, equal and greater. The sort is not stable.",
         NO),
        ("`s.forEach(visit: borrow mut fn(borrow T))`", "Calls a closure on each element of a slice, in order.", NO),
        ("`s.any(test: borrow mut fn(borrow T) -> bool) -> bool`",
         "Whether a closure returns `true` for some element of a slice; `false` for an empty slice.", NO),
        ("`s.all(test: borrow mut fn(borrow T) -> bool) -> bool`",
         "Whether a closure returns `true` for every element of a slice; `true` for an empty slice.", NO),
        ("`s.partitionPoint(test: borrow mut fn(borrow T) -> bool) -> usize`",
         "A binary search: the index of the first element for which the closure is `false`, in a slice where every `true` element comes before every `false` one.",
         NO),
    ],
))

TABLES.append(table(
    "Byte slices: `Slice<u8>` and `SliceMut<u8>`",
    "A slice of bytes has the methods of every slice, and these. In the rows, `b` is a `Slice<u8>`, `d` a "
    "`SliceMut<u8>`, and `T` a fixed-width integer type (`u8` to `u64`, `i8` to `i64`). A `Vector<u8>` has them too, on "
    "its elements. The searches are the ones a `StringView` has, with `Slice<u8>` where a view has `StringView`: "
    "[ByteSearch](https://ceesafe.github.io/CeeSafe/reference/api/core#struct-bytesearch). "
    "[Section 8.2](./ch08-02-strings) teaches the searches and the parsers on text.",
    [],
    [
        ("`b.readBE<T>(at: usize) -> T`, `b.readLE<T>(at: usize) -> T`",
         "The integer of type `T` stored in the bytes from index `at` of a byte slice, most significant byte first (`BE`) or last (`LE`).",
         INDEX + " when the integer does not lie inside the slice"),
        ("`d.writeBE(at: usize, x: T)`, `d.writeLE(at: usize, x: T)`",
         "Writes the bytes of the integer `x` into a mutable byte slice from index `at`. The type of `x` gives the width, so a literal needs a suffix (`5u32`).",
         INDEX + " when the integer does not lie inside the slice, and writes nothing"),
        ("`b.indexOf(needle: Slice<u8>) -> usize?`, `b.lastIndexOf(needle: Slice<u8>) -> usize?`",
         "The index where `needle` first or last starts in a byte slice.", "`null` when `needle` does not occur"),
        ("`b.indexOfByte(x: u8) -> usize?`", "The index of the first byte equal to `x` in a byte slice.",
         "`null` when no byte is `x`"),
        ("`b.contains(needle: Slice<u8>) -> bool`, `b.startsWith(prefix: Slice<u8>) -> bool`, `b.endsWith(suffix: Slice<u8>) -> bool`",
         "Whether a byte slice holds the given bytes anywhere, at its start, or at its end.", NO),
        ("`b.equalsIgnoreAsciiCase(other: Slice<u8>) -> bool`",
         "Whether two byte slices are equal when `A` to `Z` and `a` to `z` count as the same.", NO),
        ("`b.trim() -> Slice<u8>`, `b.trimStart() -> Slice<u8>`, `b.trimEnd() -> Slice<u8>`",
         "A byte slice without the ASCII whitespace at both ends, at its start, or at its end.", NO),
        ("`b.splitOnce(separator: Slice<u8>) -> ByteHalves?`",
         "The bytes before the first `separator` as `left` and the bytes after it as `right`.",
         "`null` when `separator` does not occur"),
        ("`b.split(separator: Slice<u8>) -> ByteSplit`",
         "An iterator over the pieces of a byte slice between separators: `for (piece in b.split(separator))`.", NO),
        ("`b.parseInt<N>() -> N?`, `b.parseIntRadix<N>(radix: u32) -> N?`",
         "The integer of type `N` that the bytes spell in decimal, or in a radix from 2 to 36.",
         "`null` for bytes that are not such a number, a value `N` cannot hold, or a radix outside 2 to 36"),
        ("`b.parseFloat<F>() -> F?`", "The `f32` or `f64` nearest to the decimal number the bytes spell.",
         "`null` for bytes that are not a number"),
        ("`StringView.fromUtf8(b: Slice<u8>) -> StringView?`",
         "The same bytes as text. Nothing is copied: the view borrows the bytes.",
         "`null` when the bytes are not valid UTF-8", ["ch12-02"]),
    ],
))

TEXT = "[TextSearch](https://ceesafe.github.io/CeeSafe/reference/api/core#struct-textsearch)"
TABLES.append(table(
    "`StringView`",
    "A `StringView` borrows UTF-8 text and cannot change it. A string literal is one. It is Copy. In the rows, `s` is "
    "a `StringView`; lengths and indexes count bytes. A `String` has every member of this table too. The searches "
    "are documented with " + TEXT + ".",
    ["ch08-02", "ch04-03"],
    [
        ("`s.length`", "The length of a text in bytes, a `usize`.", NO),
        ("`s[i]`", "Byte `i` of a text, a `u8`.", INDEX + " when `i` is not below the length", ["ch08-02"]),
        ("`s[lo..hi]`, `s[lo..]`, `s[..hi]`, `s[..]`", "A `StringView` of part of a text, by byte indexes.",
         "Panics `slice range out of bounds` for a bad range, and `string slice not on a character boundary` when a bound is inside a character",
         ["ch04-03", "ch08-02"]),
        ("`s.bytes() -> Slice<u8>`", "The bytes of a text as a slice.", NO),
        ("`s == t`, `s != t`", "Whether two texts have the same bytes.", NO, ["ch08-02"]),
        ("`StringView.fromUtf8(bytes: Slice<u8>) -> StringView?`",
         "The text that the bytes are. Nothing is copied: the view borrows the bytes.",
         "`null` when the bytes are not valid UTF-8", ["ch12-02"]),
        ("`s.indexOf(needle: StringView) -> usize?`, `s.lastIndexOf(needle: StringView) -> usize?`",
         "The byte index where `needle` first or last starts in a text.", "`null` when `needle` does not occur"),
        ("`s.indexOfByte(b: u8) -> usize?`", "The index of the first byte equal to `b` in a text.",
         "`null` when no byte is `b`"),
        ("`s.contains(needle: StringView) -> bool`, `s.startsWith(prefix: StringView) -> bool`, `s.endsWith(suffix: StringView) -> bool`",
         "Whether a text holds another anywhere, at its start, or at its end.", NO),
        ("`s.equalsIgnoreAsciiCase(other: StringView) -> bool`",
         "Whether two texts are equal when `A` to `Z` and `a` to `z` count as the same. Other letters are compared as they are.",
         NO),
        ("`s.trim() -> StringView`, `s.trimStart() -> StringView`, `s.trimEnd() -> StringView`",
         "A text without the ASCII whitespace (space, tab, line ends) at both ends, at its start, or at its end.", NO),
        ("`s.splitOnce(separator: StringView) -> TextHalves?`",
         "The text before the first `separator` as `left` and the text after it as `right`.",
         "`null` when `separator` does not occur"),
        ("`s.split(separator: StringView) -> TextSplit`",
         "An iterator over the pieces of a text between separators: `for (part in s.split(\",\"))`. `\"a,,b\"` gives `a`, an empty piece and `b`.",
         NO),
        ("`s.parseInt<T>() -> T?`", "The integer of type `T` that a text spells in decimal, with an optional sign.",
         "`null` for text that is not a number, or a value `T` cannot hold"),
        ("`s.parseIntRadix<T>(radix: u32) -> T?`", "The integer of type `T` that a text spells in a radix from 2 to 36.",
         "`null` for text that is not a number, a value `T` cannot hold, or a radix outside 2 to 36"),
        ("`s.parseFloat<T>() -> T?`",
         "The `f32` or `f64` nearest to the decimal number a text spells; `inf`, `infinity` and `nan` are accepted.",
         "`null` for text that is not a number"),
    ],
))

TABLES.append(table(
    "`String`",
    "A `String` owns UTF-8 text on the heap and can grow. It is a move type. In the rows, `t` is a `String`. A "
    "`String` also has every member of the `StringView` table (`t.length`, `t[i]`, `t[lo..hi]`, `t.bytes()`, the "
    "searches and the parsers), and a `String` argument is accepted where a `StringView` parameter is. The four "
    "formatting methods are the interface "
    "[AppendFormatted](https://ceesafe.github.io/CeeSafe/reference/api/core#interface-appendformatted).",
    ["ch08-02"],
    [
        ("`String.create() -> String`", "An empty string. It allocates nothing.", NO),
        ("`String.withCapacity(n: usize) -> Result<String, AllocError>`",
         "An empty string with room for `n` bytes.", OOM),
        ("`String.from(text: StringView) -> Result<String, AllocError>`",
         "A new string that owns a copy of `text`.", OOM),
        ("`t.clone() -> Result<String, AllocError>`", "A second string with a copy of the text of a string.", OOM),
        ("`t.append(text: StringView) -> Result<void, AllocError>`",
         "Adds `text` at the end of a string.", OOM + "; the string is unchanged"),
        ("`t.appendNumber(n) -> Result<void, AllocError>`",
         "Adds an integer or a float at the end of a string, written in decimal as `Console.write` prints it.",
         OOM + "; the string is unchanged"),
        ("`t.appendHex(value: u64, minWidth: u32, upper: bool) -> Result<void, AllocError>`",
         "Adds `value` in hexadecimal at the end of a string, with zeros before it up to `minWidth` digits, in capitals when `upper`, and no `0x`.",
         OOM),
        ("`t.appendOctal(value: u64) -> Result<void, AllocError>`",
         "Adds `value` in octal at the end of a string, with no `0o`.", OOM),
        ("`t.appendFixed(value: f64, digits: u32) -> Result<void, AllocError>`",
         "Adds a float at the end of a string with exactly `digits` digits after the point.", OOM),
        ("`t.appendPadded(text: StringView, width: usize, align: TextAlign, fill: u32) -> Result<void, AllocError>`",
         "Adds `text` at the end of a string in a field of `width` characters, filled with the character `fill` on the side that `TextAlign.Left`, `TextAlign.Right` or `TextAlign.Center` leaves.",
         OOM + "; panics `the fill is not a Unicode scalar value` for such a `fill`"),
        ("`t.view() -> StringView`", "The text of a string as a view that borrows the string.", NO),
        ("`t.clear()`", "Empties a string. The capacity stays.", NO),
        ("`t.capacity`", "The number of bytes a string has room for before it must grow, a `usize`.", NO, []),
        ("`t == u`, `t != u`",
         "Whether two strings have the same bytes. A `String` and a `StringView` are compared as `t.view() == s`.",
         NO, ["ch08-02"]),
    ],
))

TABLES.append(table(
    "Optionals: `T?`",
    "A `T?` is either `null` or `Some(value)`. In the rows, `o` is an optional of type `T?`.",
    ["ch06-03"],
    [
        ("`null`, `Some(v)`",
         "The two values of an optional. Where a `T?` is expected, a plain `T` value converts to `Some(value)`.", NO,
         ["ch06-03"]),
        ("`o.isSome() -> bool`, `o.isNull() -> bool`", "Whether an optional holds a value, or holds none.", NO),
        ("`o == null`, `o != null`", "Whether an optional holds no value, or holds one.", NO, ["ch06-03"]),
        ("`o ?? fallback`",
         "The value of an optional, or `fallback` when it is `null`. `fallback` is evaluated only then.", NO,
         ["ch06-03"]),
        ("`o!`", "The value of an optional, for when `null` would be a mistake in the program.",
         "Panics `unwrapped null` when the optional is `null`", ["ch06-03", "ch09-01"]),
        ("`o?`", "The value of an optional; on `null`, the enclosing function, which returns an optional, returns `null`.",
         "Returns `null` from the function", ["ch06-03"]),
        ("`match (o) { Some(v) => { } null => { } }`",
         "Runs one arm for an optional with a value, bound to `v`, and the other for `null`.", NO, ["ch06-03"]),
        ("`o.okOr(e: E) -> Result<T, E>`", "An optional as a `Result`: `Ok` of the value, or `Err(e)` for `null`.",
         "`Err(e)` when the optional is `null`", ["ch09-02"]),
        ("`o.asBorrow() -> borrow T?`",
         "An optional borrow of the value inside an optional that stays where it is.", NO),
        ("`o.asBorrowMut() -> (borrow mut T)?`",
         "An optional mutable borrow of the value inside an optional variable, to change the value in place.", NO),
    ],
    first="Form",
))

TABLES.append(table(
    "`Result<T, E>`",
    "A `Result<T, E>` is either `Ok(value)` or `Err(error)`. In the rows, `r` is a result. The methods are documented "
    "with [ResultMethods](https://ceesafe.github.io/CeeSafe/reference/api/core#struct-resultmethods).",
    ["ch09-02"],
    [
        ("`Ok(v)`, `Err(e)`, `Ok()`",
         "The two values of a result. `Ok()` is the success of a `Result<void, E>`.", NO, ["ch09-02"]),
        ("`r.isOk() -> bool`, `r.isErr() -> bool`", "Whether a result is `Ok`, or is `Err`.", NO),
        ("`r.ok() -> T?`", "The `Ok` value of a result as an optional. The result is consumed.",
         "`null` when the result is `Err`"),
        ("`r.err() -> E?`", "The `Err` value of a result as an optional. The result is consumed.",
         "`null` when the result is `Ok`"),
        ("`r?`",
         "The `Ok` value of a result; on `Err`, the enclosing function returns that error. A variant marked `@from` converts the error to the function's error type.",
         "Returns the `Err` from the function", ["ch09-02"]),
        ("`r!`", "The `Ok` value of a result, for when an error would be a mistake in the program.",
         "Panics `unwrapped an Err value` when the result is `Err`", ["ch09-02", "ch09-01"]),
        ("`match (r) { Ok(v) => { } Err(e) => { } }`",
         "Runs one arm for a successful result, with its value, and the other for a failed one, with its error.", NO,
         ["ch09-02"]),
    ],
    first="Form",
))

TABLES.append(table(
    "The built-in error types",
    "Two enums are built in, for the two kinds of failure the built-in types report.",
    ["ch09-03", "ch12-02"],
    [
        ("`AllocError.OutOfMemory`",
         "The one value of `AllocError`: memory could not be allocated. It is the error of `push`, `append`, `Box.create` and every other call that allocates.",
         NO, ["ch09-03"]),
        ("`IoError.NotFound`, `IoError.PermissionDenied`, `IoError.AlreadyExists`, `IoError.IsDirectory`, `IoError.NotDirectory`, `IoError.InvalidInput`, `IoError.StorageFull`, `IoError.TooManyOpenFiles`, `IoError.OutOfMemory`, `IoError.Other(i32)`",
         "The variants of `IoError` for files, storage and failures of any kind. `Other` holds the platform's error number.",
         NO, ["ch12-02"]),
        ("`IoError.BrokenPipe`, `IoError.ConnectionRefused`, `IoError.AddressInUse`, `IoError.ConnectionReset`, `IoError.ConnectionAborted`, `IoError.NotConnected`, `IoError.TimedOut`, `IoError.HostUnreachable`, `IoError.NetworkUnreachable`, `IoError.AddressNotAvailable`, `IoError.Unsupported`, `IoError.HostNotFound`",
         "The variants of `IoError` for pipes, sockets and the network.", NO, []),
        ("`e.message() -> StringView`",
         "A fixed English text for an `IoError`, the same on every platform: `No such file or directory` for `NotFound`.",
         NO, ["ch12-02"]),
    ],
    first="Form",
))

TABLES.append(table(
    "`Box<T>`",
    "A `Box<T>` owns one value on the heap. It is a move type, and destroying it destroys the value. A box is used "
    "as its contents are: `b.field`, `b.method()` and `b[i]` reach the value, and a `Box<T>` argument is accepted "
    "for a `borrow T` parameter. In the rows, `b` is a box.",
    ["ch15-01", "ch15-03"],
    [
        ("`Box.create(v: T) -> Result<Box<T>, AllocError>`", "Moves `v` to the heap and returns the box that owns it.",
         OOM + "; `v` is destroyed"),
        ("`Box.createIn(a: A, v: T) -> Result<Box<T, A>, AllocError>`",
         "Moves `v` into memory from the allocator `a`, a value of an allocator type such as `FailingAllocator {}`.",
         OOM + "; `v` is destroyed"),
        ("`Box.intoRaw(b: Box<T>) -> *mut T`",
         "Gives up a box and returns the address of its contents. Nothing is freed: the program owns the memory until it gives the address back.",
         NO, ["ch20-01"]),
        ("`Box.fromRaw(p: *mut T) -> Box<T>`",
         "A box again from the address `Box.intoRaw` returned. It is unsafe, so it is written in an `unsafe` block, and the expected type names the box type.",
         "Not checked: any other address is undefined behaviour", ["ch20-01"]),
    ],
))

TABLES.append(table(
    "`Vector<T>`",
    "A `Vector<T>` owns a growable list of values on the heap. It is a move type. In the rows, `v` is a vector. A "
    "vector also has the members of the slice table on its elements (`v.swap`, `v.sortBy`, `v.forEach`, `v.any`, "
    "`v.all`, `v.partitionPoint`, `v.splitAt`, `v.splitAtMut`), a `Vector<u8>` those of the byte slice table, and a "
    "vector argument is accepted where a `Slice<T>` parameter is. The library's part is documented with "
    "[VectorEditing](https://ceesafe.github.io/CeeSafe/reference/api/core#struct-vectorediting), "
    "[VectorCopying](https://ceesafe.github.io/CeeSafe/reference/api/core#struct-vectorcopying) and "
    "[VectorBytes](https://ceesafe.github.io/CeeSafe/reference/api/core#struct-vectorbytes).",
    ["ch08-01", "ch09-03", "ch12-02"],
    [
        ("`Vector<T>.create() -> Vector<T>`", "An empty vector. It allocates nothing.", NO),
        ("`Vector<T>.withCapacity(n: usize) -> Result<Vector<T>, AllocError>`",
         "An empty vector with room for `n` elements.", OOM),
        ("`v.length`", "The number of elements in a vector, a `usize`.", NO),
        ("`v.capacity`", "The number of elements a vector has room for before it must grow, a `usize`.", NO),
        ("`v[i]`", "Element `i` of a vector, to read, to borrow, or (in a `var`) to assign.",
         INDEX + " when `i` is not below the length", ["ch08-01"]),
        ("`v.push(x: T) -> Result<void, AllocError>`", "Adds `x` at the end of a vector, growing it when it is full.",
         OOM + "; the vector is unchanged and `x` is destroyed"),
        ("`v.pop() -> T?`", "Takes the last element out of a vector.", "`null` when the vector is empty"),
        ("`v.insert(at: usize, x: T) -> Result<void, AllocError>`",
         "Puts `x` at index `at` of a vector and moves the later elements up one place.",
         OOM + "; panics `index out of bounds` when `at` is above the length"),
        ("`v.remove(at: usize) -> T`",
         "Takes the element at index `at` out of a vector and moves the later elements down one place.",
         INDEX + " when `at` is not below the length"),
        ("`v.swapRemove(at: usize) -> T`",
         "Takes the element at index `at` out of a vector and puts the last element in its place: one move, and the order changes.",
         INDEX + " when `at` is not below the length"),
        ("`v.clear()`", "Destroys every element of a vector. The capacity stays.", NO),
        ("`v.truncate(n: usize)`",
         "Destroys the elements of a vector from index `n` on. A vector that is already that short is unchanged.", NO),
        ("`v.retain(keep: borrow mut fn(borrow T) -> bool)`",
         "Keeps the elements of a vector for which a closure returns `true`, in order, and destroys the others.", NO),
        ("`v.extend(s: Slice<T>) -> Result<void, AllocError>`",
         "Adds a copy of every element of `s` at the end of a vector. `T` is Copy.", OOM + "; the vector is unchanged"),
        ("`v.resize(n: usize, fill: T) -> Result<void, AllocError>`",
         "Makes the length of a vector `n`: adds copies of `fill`, or destroys the elements from `n` on. `T` is Copy.",
         OOM + "; the vector is unchanged"),
        ("`v.reserve(extra: usize) -> Result<void, AllocError>`",
         "Makes room in a vector for `extra` more elements, so that adding them does not allocate.",
         OOM + "; the vector is unchanged"),
        ("`v.clone() -> Result<Vector<T>, AllocError>`",
         "A second vector with a copy of the elements of a vector. `T` is Copy.", OOM),
        ("`v.view() -> Slice<T>`", "The elements of a vector as a slice that borrows the vector.", NO),
        ("`v.viewMut() -> SliceMut<T>`", "The elements of a vector as a mutable slice, for `fill` and `copyFrom`.", NO),
        ("`for (x in v) { }`",
         "Runs the body once for each element of a vector: `x` is a copy of a Copy element, and a borrow of any other.",
         NO, ["ch08-01"]),
        ("`v.pushBE(x: N) -> Result<void, AllocError>`, `v.pushLE(x: N) -> Result<void, AllocError>`",
         "Adds the bytes of the fixed-width integer `x` at the end of a `Vector<u8>`, most significant byte first (`BE`) or last (`LE`).",
         OOM + "; the vector is unchanged"),
        ("`v.appendHex(value: u64, minWidth: u32, upper: bool)`, `v.appendOctal(value: u64)`, `v.appendFixed(value: f64, digits: u32)`, `v.appendPadded(text: StringView, width: usize, align: TextAlign, fill: u32)`",
         "The formatting methods of a `String`, on a `Vector<u8>`: each adds the formatted text's bytes and returns a `Result<void, AllocError>`.",
         OOM),
        ("`Vector.intoRaw(v: Vector<T>) -> VectorParts<T>`",
         "Gives up a vector and returns its `pointer`, `length` and `capacity`. Nothing is freed.", NO, []),
        ("`Vector.fromRaw(pointer: *mut T, length: usize, capacity: usize) -> Vector<T>`",
         "A vector again from the parts `Vector.intoRaw` returned. It is unsafe, and the expected type names the vector type.",
         "Not checked: other parts are undefined behaviour", []),
        ("`v.setLength(n: usize)`",
         "Sets the length of a vector over elements that unsafe code wrote into its spare capacity. It is unsafe.",
         INDEX + " when `n` is above the capacity"),
        ("`Vector.intoForeign(v: Vector<T>) -> VectorParts<T>`, `Vector.fromForeign(pointer: *mut T, length: usize, capacity: usize) -> Vector<T>`",
         "`intoRaw` and `fromRaw` for a buffer of Copy values that a host program written in C adopts, or gives up. Both are unsafe.",
         "Not checked", []),
    ],
))

TABLES.append(table(
    "`HashMap<K, V>`",
    "A `HashMap<K, V>` owns values found by key; it is in lib/std, and a hosted program uses it without an import. "
    "In the rows, `m` is a map. The key type implements `HashKey`: the fixed-width integers, `usize`, `bool`, "
    "`String` and `Vector<u8>` do, and `@derive(Equal, HashKey)` gives it to a struct. Reference: "
    "[HashMap](https://ceesafe.github.io/CeeSafe/reference/api/std#struct-hashmap).",
    ["ch08-03"],
    [
        ("`HashMap<K, V>.create() -> HashMap<K, V>`", "An empty hash map. It allocates nothing.", NO),
        ("`HashMap<K, V>.withSeed(k0: u64, k1: u64) -> HashMap<K, V>`",
         "An empty hash map whose hash is keyed by the two given numbers, not by the process's random key.", NO),
        ("`m.put(key: K, value: V) -> Result<V?, AllocError>`",
         "Puts `value` at `key` in a hash map, and returns the value that was there, if one was.",
         OOM + "; the map is unchanged"),
        ("`m.get(key: borrow K) -> borrow V?`", "A borrow of the value at `key` in a hash map.",
         "`null` when the key is not there"),
        ("`m.getMut(key: borrow K) -> borrow mut V?`", "A mutable borrow of the value at `key` in a hash map, to change it.",
         "`null` when the key is not there"),
        ("`m.contains(key: borrow K) -> bool`", "Whether `key` is in a hash map.", NO),
        ("`m.remove(key: borrow K) -> V?`",
         "Takes `key` and its value out of a hash map and returns the value. The last entry takes the removed one's place in the order.",
         "`null` when the key is not there"),
        ("`m.length() -> usize`", "The number of entries in a hash map. It is a method, so it has parentheses.", NO),
        ("`m.keyAt(i: usize) -> borrow K`, `m.valueAt(i: usize) -> borrow V`",
         "The key or the value of entry `i` of a hash map, for `i` in `0..m.length()`. Entries are in insertion order.",
         INDEX + " when `i` is not below the length"),
        ("`m.clear()`", "Takes every entry out of a hash map.", NO),
    ],
))

TABLES.append(table(
    "`OrderedMap<K, V>`",
    "An `OrderedMap<K, V>` owns values found by key and keeps them in the order of the keys; it is in lib/std. In "
    "the rows, `m` is an ordered map and `at` an `OrderedCursor`, a position in it. The key type implements "
    "`OrderKey`: the integers, `bool`, `String` and `Vector<u8>` do, and `@derive(OrderKey)` gives it to a struct. A "
    "cursor is good until the map changes. Reference: "
    "[OrderedMap](https://ceesafe.github.io/CeeSafe/reference/api/std#struct-orderedmap).",
    ["ch08-03"],
    [
        ("`OrderedMap<K, V>.create() -> OrderedMap<K, V>`", "An empty ordered map.", NO),
        ("`m.put(key: K, value: V) -> Result<V?, AllocError>`",
         "Puts `value` at `key` in an ordered map, and returns the value that was there, if one was.",
         OOM + "; the map is unchanged"),
        ("`m.get(key: borrow K) -> borrow V?`", "A borrow of the value at `key` in an ordered map.",
         "`null` when the key is not there"),
        ("`m.getMut(key: borrow K) -> borrow mut V?`",
         "A mutable borrow of the value at `key` in an ordered map, to change it.", "`null` when the key is not there"),
        ("`m.contains(key: borrow K) -> bool`", "Whether `key` is in an ordered map.", NO),
        ("`m.remove(key: borrow K) -> V?`", "Takes `key` and its value out of an ordered map and returns the value.",
         "`null` when the key is not there"),
        ("`m.length() -> usize`, `m.isEmpty() -> bool`",
         "The number of entries in an ordered map, and whether there are none.", NO),
        ("`m.clear()`", "Takes every entry out of an ordered map.", NO),
        ("`m.first() -> OrderedCursor?`, `m.last() -> OrderedCursor?`",
         "A cursor at the entry of an ordered map with the least key, or the greatest.", "`null` when the map is empty"),
        ("`m.seek(key: borrow K) -> OrderedCursor?`",
         "A cursor at the first entry of an ordered map whose key is `key` or comes after it.",
         "`null` when there is no such entry"),
        ("`m.seekAfter(key: borrow K) -> OrderedCursor?`",
         "A cursor at the first entry of an ordered map whose key comes after `key`.",
         "`null` when there is no such entry"),
        ("`m.next(at: OrderedCursor) -> OrderedCursor?`, `m.previous(at: OrderedCursor) -> OrderedCursor?`",
         "A cursor at the entry after or before the cursor's entry, in key order.",
         "`null` at the end or the start; panics `ordered map changed under a cursor` for a cursor made before a change"),
        ("`m.keyAt(at: OrderedCursor) -> borrow K`, `m.valueAt(at: OrderedCursor) -> borrow V`",
         "The key or the value of the entry a cursor is at.",
         "Panics `ordered map changed under a cursor` for a cursor made before a change"),
        ("`m.valueAtMut(at: OrderedCursor) -> borrow mut V`",
         "A mutable borrow of the value of the entry a cursor is at, to change it.",
         "Panics `ordered map changed under a cursor` for a cursor made before a change"),
    ],
))

TABLES.append(table(
    "Ranges",
    "A range is written `lo..hi` and is not a value: it cannot be stored in a variable or passed to a function. It "
    "has three uses.",
    ["ch03-05"],
    [
        ("`for (i in lo..hi) { }`",
         "Runs the body with `i` from `lo` up to but not including `hi`. Both bounds have one integer type; two plain literals make a `usize` range when the body indexes with `i`, and an `i32` range otherwise.",
         "No: a range with `lo >= hi` runs the body no times", ["ch03-05"]),
        ("`a[lo..hi]`, `a[lo..]`, `a[..hi]`, `a[..]`",
         "Slices an array, a slice, a vector, a `StringView` or a `String` from index `lo` up to but not including `hi`. The bounds are `usize`.",
         "Panics `slice range out of bounds` unless `lo <= hi` and `hi` is at most the length", ["ch04-03"]),
        ("`lo..=hi`",
         "In a pattern only: matches an integer or a character from `lo` up to and including `hi`, as in `'a'..='z' => { }`.",
         NO, ["ch19-02"]),
    ],
    first="Form",
))

TABLES.append(table(
    "Iterators",
    "`for (x in e)` accepts an array, a slice, a vector, and any struct that has a method "
    "`fn next(borrow mut this) -> T?`. lib/core names that shape as the interface "
    "[Iterator](https://ceesafe.github.io/CeeSafe/reference/api/core#interface-iterator). There are no adapter methods "
    "such as `map` or `filter`: a loop does that work.",
    ["ch13-02", "ch08-02"],
    [
        ("`it.next() -> T?`",
         "The next value of an iterator. `for (x in it)` calls it until it returns `null`.",
         "`null` when the iterator has no more values"),
        ("`TextSplit`",
         "The iterator that `s.split(separator)` returns for a `StringView` or a `String`. Its values are `StringView` pieces.",
         NO, ["ch08-02", "ch13-02"]),
        ("`ByteSplit`",
         "The iterator that `b.split(separator)` returns for a `Slice<u8>`. Its values are `Slice<u8>` pieces.", NO,
         []),
        ("`TextHalves`, `ByteHalves`",
         "The struct that `splitOnce` returns: two public fields, `left` and `right`, each a `StringView` (or a `Slice<u8>`).",
         NO, ["ch08-02"]),
    ],
))

TABLES.append(table(
    "`Console`",
    "`Console` is standard input, standard output and standard error. Its functions are called on the type. They "
    "block the thread, so an `async fn` uses lib/std's `AsyncConsole`.",
    ["ch01-02", "ch02-00", "ch12-06", "ch21-01", "ch12-02"],
    [
        ("`Console.write(x)`",
         "Writes an integer, a float, a `bool`, a `StringView` or a `String` to standard output.", NO),
        ("`Console.writeLine(x)`, `Console.writeLine()`",
         "Writes a value to standard output and ends the line; with no argument, only ends the line.", NO),
        ("`Console.writeBytes(bytes: Slice<u8>)`",
         "Writes bytes to standard output as they are, valid UTF-8 or not.", NO),
        ("`Console.writeError(text: StringView)`, `Console.writeErrorLine(text: StringView)`",
         "Writes text to standard error, without or with a line end.", NO),
        ("`Console.flush()`",
         "Writes what standard output has buffered now, so that it is out before the program waits or is stopped.", NO),
        ("`Console.read(mut buffer) -> Result<usize, IoError>`",
         "Reads bytes from standard input into `buffer`, a `SliceMut<u8>`, and returns how many; 0 at the end of the input.",
         "`Err(IoError)` when the read fails", []),
        ("`Console.readLine(mut line) -> Result<bool, IoError>`",
         "Reads the next line of standard input into `line`, a `String` it empties first, without the line end; `false` at the end of the input.",
         "`Err(IoError.InvalidInput)` for a line that is not UTF-8"),
    ],
    first="Function",
))

TABLES.append(table(
    "`Process`",
    "`Process` is the running program: its arguments, its exit, and the programs it starts.",
    ["ch12-01", "ch03-03"],
    [
        ("`Process.argumentCount() -> usize`",
         "The number of command-line arguments, counting the program's name, which is argument 0.", NO),
        ("`Process.argument(i: usize) -> StringView?`", "Command-line argument `i` as text.",
         "`null` when there is no argument `i`, or its bytes are not UTF-8"),
        ("`Process.exit(status: i32) -> never`", "Ends the program now with the exit status `status`.",
         "Does not return"),
        ("`Process.run(program: StringView, arguments: borrow Vector<StringView>) -> Result<i32, IoError>`",
         "Starts a program found on `PATH`, waits for it, and returns its exit status.",
         "`Err(IoError)` when the program cannot be started", []),
        ("`Process.capture(program: StringView, arguments: borrow Vector<StringView>, input: Slice<u8>?) -> Result<ProcessOutput, IoError>`",
         "Runs a program with `input` as its standard input, and returns its `status`, and its `stdout` and `stderr` as byte vectors.",
         "`Err(IoError)` when the program cannot be started", []),
    ],
    first="Function",
))

TABLES.append(table(
    "Built-in functions",
    "These functions are part of the compiler and are called by name alone. A function of the package with one of "
    "these names takes its place.",
    ["ch03-02", "ch20-04", "ch09-01", "ch14-01", "ch20-05"],
    [
        ("`x as T`",
         "Converts a number to a type that holds every value of its own type (`u8` to `u32`, `i32` to `f64`, `bool` to an integer). Any other conversion is a compile error.",
         NO, ["ch03-02"]),
        ("`truncate<T>(x) -> T`",
         "Converts an integer to the integer type `T`, keeping the low bits. For a float `x`, rounds toward zero.",
         "An integer: no. A float: panics `float out of range` for NaN, an infinity, or a value `T` cannot hold"),
        ("`checkedTruncate<T>(x) -> T?`",
         "Converts an integer or a float to the integer type `T` when `T` can hold the value.",
         "`null` when `T` cannot hold the value"),
        ("`saturatingTruncate<T>(x) -> T`",
         "Converts an integer or a float to the integer type `T`, or to the least or greatest value of `T` when the value is beyond it. NaN gives 0.",
         NO),
        ("`round<T>(x) -> T`",
         "Converts an integer or a float to the float type `T`, rounded to the nearest value `T` has.", NO),
        ("`panic(message: StringView) -> never`", "Stops the program with a message and the place in the source.",
         "Always panics"),
        ("`assert(condition: bool)`, `assert(condition: bool, message: StringView)`",
         "Checks a condition in every build.",
         "Panics `assertion failed`, or with the message, when the condition is `false`"),
        ("`debugAssert(condition: bool)`, `debugAssert(condition: bool, message: StringView)`",
         "Checks a condition in debug builds, in tests and in the interpreter. A release build leaves the call out, so its arguments may change nothing.",
         "Panics as `assert` does, where it is checked"),
        ("`replace(mut place, value: T) -> T`",
         "Puts `value` in a variable, field or element and returns the value that was there.", NO, []),
        ("`swap(mut a, mut b)`", "Exchanges the values of two places of one type.", NO, []),
        ("`embed(path) -> Slice<u8>`, `embedText(path) -> StringView`",
         "The bytes, or the UTF-8 text, of a file of the package, read when the program is compiled. `path` is a string literal, relative to the source file.",
         "A compile error (N0362) for a path that is missing or leads out of the package"),
        ("`layoutOf<T>() -> Layout`", "The size and alignment of the type `T` in bytes, as the fields `size` and `align`.",
         NO),
        ("`offsetOf<T>(field) -> usize`", "The offset in bytes of a field in the struct type `T`.", NO),
        ("`callerAddress() -> usize`",
         "In the body of an `@export` function: the address its C caller returns to; 0 when CeeSafe code calls the function.",
         NO),
    ],
    first="Function",
))


SITE = "https://ceesafe.github.io/CeeSafe"

HEAD = """---
title: "Appendix C: Built-in types and their methods"
summary: A reference table for each type a program uses without declaring it - integers, floats, arrays, slices, StringView, String, optionals, Result, Box, Vector, HashMap, OrderedMap, ranges, iterators, Console, Process and the built-in functions - with each member's signature, what it does, how it fails and the section that teaches it.
keywords: [built-in types, methods, signatures, integer methods, float methods, array, Slice, SliceMut, StringView, String, optional, Result, AllocError, IoError, Box, Vector, HashMap, OrderedMap, range, Iterator, Console, Process, truncate, checkedTruncate, saturatingTruncate, round, assert, debugAssert, panic, replace, swap, embed]
---

# Appendix C: Built-in types and their methods

This appendix lists the types a CeeSafe program uses without declaring them, and the members of each: the numbers,
arrays, slices, `StringView`, `String`, optionals, `Result`, `Box`, `Vector`, the two maps, ranges, iterators,
`Console`, `Process`, and the functions that are called by name alone. Find the type's heading, then the member's
row. The chapters teach these types; the tables are for looking a member up once you know what you are looking for.

## How to read a row

- **The first column** is the call as you write it, with the type of each argument and, after `->`, the type of the
  result. The sentence above each table says what the receiver's letter stands for (`v` is a vector, `s` a
  `StringView`). A call with no `->` returns nothing.
- **Fails or panics** says what happens when the call cannot do its work. *No* means it always succeeds. A `null` or
  an `Err` is a failure as a value, which the program handles with `??`, `?`, `!` or `match`
  ([section 6.3](./ch06-03-optionals), [section 9.2](./ch09-02-result)). *Panics* means the program stops with the
  message shown ([section 9.1](./ch09-01-panics)).
- **Taught in** names the section that teaches the member. A dash means no chapter does: the reference linked above
  the table, or the page of [built-in types and functions](https://ceesafe.github.io/CeeSafe/guide/builtins) on the
  documentation site, has the rest.

The program below uses four of the rows: `v.pop()` returns a `T?`, `checkedTruncate` a `T?`, `saturatingTruncate` a
`T`, and `s.indexOf` a `usize?`.

```ceesafe
fn main() {
    var names = Vector<StringView>.create();
    names.push("Ada")!;
    names.push("Grace")!;
    let last = names.pop() ?? "nobody";
    Console.writeLine(last);

    let wide: i64 = 300;
    Console.writeLine(checkedTruncate<u8>(wide) ?? 0);
    Console.writeLine(saturatingTruncate<u8>(wide));

    let at = "key=value".indexOf("=");
    match (at) {
        Some(i) => { Console.writeLine(i); }
        null => { Console.writeLine("no '='"); }
    }
}
```

```text
Grace
0
255
3
```

When a call names a method that a vector, a string, a slice, an optional, a result or a box does not have, the
compiler's message lists the ones the type does have, so a wrong guess is corrected at the first check.

## The scalar types

A scalar type holds one value. Every scalar type is Copy.

| Type | Values | Notes | Taught in |
|---|---|---|---|
| `i8`, `i16`, `i32`, `i64` | Signed integers of 8, 16, 32 and 64 bits. | A whole-number literal is an `i32` when nothing else decides its type. | S32 |
| `u8`, `u16`, `u32`, `u64` | Unsigned integers of 8, 16, 32 and 64 bits, from 0 up. | The bit operators and the shifts take unsigned integers only. A `u8` is a byte. | S32 |
| `isize`, `usize` | A signed and an unsigned integer as wide as an address of the target, 64 bits on every supported target. | `usize` is the type of every length and index. | S32 |
| `f32`, `f64` | IEEE 754 floating-point numbers of 32 and 64 bits. | A literal with a point or an exponent is an `f64` when nothing else decides its type. | S32 |
| `bool` | `true` and `false`. | It has no methods. `b as u8` is 0 or 1. | S32 |
| a character literal, `'A'` | An integer, the character's Unicode number. There is no `char` type. | It takes the integer type that is expected, and is a `u32` otherwise. In a `u8` it must be ASCII. | S32 |
| `void` | No value. | The success type of a result that carries no value, `Result<void, E>`, whose value is `Ok()`. A function that returns nothing is written without `->`. | S92 |
| `never` | No value. | The return type of a function that does not return, such as `Process.exit`. | S33 |
"""
HEAD_SECTIONS = {"S32": "ch03-02", "S92": "ch09-02", "S33": "ch03-03"}  # the sections HEAD's last table names


def rest(rows):
    out = ["## Where the rest is documented", "",
           "The libraries have more types than this appendix lists. The chapters teach the ones below, and the "
           "documentation site has every member of each: the "
           f"[built-in types and functions]({SITE}/guide/builtins) of the compiler, "
           f"[lib/core]({SITE}/reference/api/core) and [lib/std]({SITE}/reference/api/std). With the MCP server, "
           "`ceesafe_api` looks up any of these names.", "",
           "| Types | What they are for | Taught in | Reference |", "|---|---|---|---|"]
    for types, what, sections, reference in rows:
        taught_in = ", ".join(label(page(s)) for s in sections) if sections else "–"
        out.append(f"| {types} | {what} | {taught_in} | {reference} |")
    return "\n".join(out) + "\n"


def link(text, path):
    return f"[{text}]({SITE}/{path})"


REST_ROWS = [
    ("`File`, `Directory`, `Environment`", "Files, directories and environment variables.", ["ch12-02", "ch12-05"],
     link("built-in types", "guide/builtins#file-tcplistener-and-tcpstream") + ", " + link("lib/std", "reference/api/std#struct-environment")),
    ("`TcpListener`, `TcpStream`", "TCP sockets that block the thread.", ["ch21-01"],
     link("built-in types", "guide/builtins#file-tcplistener-and-tcpstream")),
    ("`Random`, `Clock`, `UnixTime`", "Random numbers and the time of day.", ["ch02-00"],
     link("lib/std", "reference/api/std#struct-random")),
    ("`SystemHeap`, `FailingAllocator`, `Arena`, `FixedBufferAllocator`, `Scratch`, `Pool<T>`",
     "Allocators, and regions that free everything at once.", ["ch09-03", "ch15-03"], link("memory", "guide/memory")),
    ("`SlotMap<T>`, `Handle<T>`, `StableVector<T>`", "Collections whose elements are named by a handle or stay in place.",
     ["ch15-05"], link("collections", "guide/collections")),
    ("`Cell<T>`, `RefCell<T>`, `OnceCell<T>`", "Values that change through a shared borrow.", ["ch15-04"],
     link("lib/core", "reference/api/core#struct-cell")),
    ("`Arc<T>`, `Mutex<T>`, `OnceLock<T>`, `AtomicU64` and the other atomics", "Values that several owners or threads share.",
     ["ch15-05", "ch16-03"],
     link("lib/std", "reference/api/std#struct-arc") + ", " + link("lib/core", "reference/api/core#struct-atomicu64")),
    ("`Thread`, `Channel<T>`", "Threads, and queues between them.", ["ch16-01", "ch16-02"], link("threads", "guide/threads")),
    ("`Async`, `Timer`, `TaskSet`, `AsyncError`, `AsyncConsole`, `AsyncFiles`, `AsyncTcpListener`, `AsyncTcpStream`",
     "Async tasks, scopes, timeouts, and input and output that waits without blocking a thread.",
     ["ch17-01", "ch17-02", "ch17-03", "ch17-04"], link("async", "guide/async")),
    ("`Check`, `TestFailure`", "The checks a `@test` function makes.", ["ch11-01"],
     link("lib/std", "reference/api/std#struct-check")),
    ("`*T`, `*mut T`, `Ptr`, `CStr`", "Raw pointers and C strings, for unsafe code and calls of C.", ["ch20-01", "ch20-02"],
     link("unsafe code and C", "guide/unsafe-and-c")),
    ("`TextWriter`, `ByteOrder`, `VectorBytes`, `NumberParsing`, `Formatted`",
     "Text formatted into a fixed buffer without allocating, and the static functions behind the byte and text methods of this appendix.",
     [], link("lib/core", "reference/api/core#struct-textwriter")),
    ("`Layout`, `Allocator`, `Volatile<T>`, `MmioPtr`, `Cpu`", "Programs without an operating system: allocators of your own, device registers, the processor.",
     ["ch22-01", "ch22-02"], link("kernel code", "guide/kernel")),
    ("`Crc32c`, `Sha256`, `HmacSha256`, `Md5`, `AsyncUdpSocket`, `AsyncDns`, `Window`",
     "Checksums and hashes, UDP and name lookup, native windows.", [], link("lib/std", "reference/api/std")),
]


# The structs of lib/core and lib/std that hold the built-in types' methods. The rows give their members' signatures
# as a program writes the call, so they cannot be copied from the API pages; API_REVIEWED records the signatures the
# rows were written from, and the page is not written while spec/api/ says otherwise.
API_PAGES = ["spec/api/core.md", "spec/api/std.md"]
API_STRUCTS = ["TextSearch", "ByteSearch", "ResultMethods", "OptionalResults", "SliceSplitting", "SliceAlgorithms",
               "VectorEditing", "VectorCopying", "StringCopying", "VectorRaw", "VectorForeign", "VectorBytes",
               "NumberParsing", "ByteOrder", "AppendFormatted", "HashMap", "OrderedMap"]
API_REVIEWED = book_appendix_gen.INPUTS + "/appendix-c.api.txt"


def api_signatures(root):
    """The lines `Struct.member<tab>signature` of API_STRUCTS, as the pages `ceesafec doc` wrote have them."""
    lines = []
    for path in API_PAGES:
        parts = re.split(r"^(##+ .*)$", book_appendix_gen.read(root, path).replace("\r\n", "\n"), flags=re.M)
        current = None
        for i in range(1, len(parts), 2):
            heading, body = parts[i], parts[i + 1]
            block = re.search(r"```ceesafe\n(.*?)```", body, re.S)
            signature = " ".join(block.group(1).split()) if block else ""
            if heading.startswith("## "):
                current = heading[3:].split()[-1]
                name = current
            elif heading.startswith("### "):
                member = heading[4:].strip()
                name = current + "." + (member.split(".", 1)[1] if "." in member else member)
            else:
                continue
            if current in API_STRUCTS:
                lines.append(f"{name}\t{signature}")
    found = {line.split("\t")[0].split(".")[0] for line in lines}
    missing = [struct for struct in API_STRUCTS if struct not in found]
    book_appendix_gen.need(not missing, f"spec/api/ has no {', '.join(missing)}, which API_STRUCTS ({SCRIPT}) names")
    return sorted(lines)


def api_reviewed(root):
    """Nothing when spec/api/ has the signatures the rows were written from; else the page cannot be written."""
    now = api_signatures(root)
    then = book_appendix_gen.read(root, API_REVIEWED).replace("\r\n", "\n").strip("\n").split("\n")
    if now == then:
        return
    old, new = dict(line.split("\t", 1) for line in then if "\t" in line), dict(line.split("\t", 1) for line in now)
    said = []
    for name in sorted(set(old) | set(new)):
        if name not in old:
            said.append(f"{name} is new (`{new[name]}`)")
        elif name not in new:
            said.append(f"{name} is gone")
        elif old[name] != new[name]:
            said.append(f"{name} is now `{new[name]}`, and was `{old[name]}`")
    more = f", and {len(said) - 3} more" if len(said) > 3 else ""
    raise book_appendix_gen.CannotWrite(
        f"spec/api/ no longer has the signatures the rows were written from ({API_REVIEWED}): "
        f"{'; '.join(said[:3]) or 'the lines are not in order'}{more}. Correct the members' rows in {SCRIPT} where"
        f" they say otherwise, and record the signatures with `python3 {SCRIPT} --api`")


def render(root):
    """The page's text, without the note that it is generated."""
    api_reviewed(root)
    load(root)
    head = HEAD
    for mark, section in HEAD_SECTIONS.items():
        head = head.replace(mark, label(page(section)))
    return head + "\n" + "\n".join(written(*one) for one in TABLES) + "\n" + rest(REST_ROWS)


def main():
    if sys.argv[1:] == ["--api"]:
        lines = api_signatures(book_appendix_gen.ROOT)
        with open(os.path.join(book_appendix_gen.ROOT, *API_REVIEWED.split("/")), "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines) + "\n")
        print(f"{API_REVIEWED}: written ({len(lines)} signatures)")
        return 0
    return book_appendix_gen.run(sys.modules[__name__], __doc__)


if __name__ == "__main__":
    sys.exit(main())
