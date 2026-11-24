# Formatting and documentation

## Formatting

`ceesafec fmt` rewrites CeeSafe source in one canonical style. It has no options, so code formatted by anyone looks the
same. The style is specified in [Formatting](../spec/formatting.md).

```sh
ceesafec fmt                       # the package's sources (or: ceesafec fmt file.cee dir/)
ceesafec fmt --check               # change nothing; list the files that would change; exit 1 if any
ceesafec fmt --stdout file.cee    # print one file formatted
```

The style, briefly:

- 4-space indentation, no tabs, at most one blank line in a row, lines up to 120 columns;
- a block or list you wrote on one line stays on one line if it fits; one you broke across lines is laid out one
  element per line, with a trailing comma;
- a line that is too long is broken at the outermost place that helps: an expression body, then a list, then an
  initialiser, then an operator;
- `} else {` on one line, and the blocks of an `if`/`else` chain are broken together;
- trailing comments on consecutive lines are aligned.

It is safe by construction: the formatter only changes whitespace and trailing commas, and before returning it parses
its own output and checks that the program and its comments are unchanged. A file with a syntax error is left alone.

## Documentation

`ceesafec doc` writes one Markdown page for a package's public items: each item's signature, as the formatter prints
it, and its `///` doc comment, which is Markdown.

```sh
ceesafec doc -o api.md --title "geometry"
ceesafec doc --missing             # list the public items without a doc comment; exit 1 if any
ceesafec doc ByteString.fromString # the library reference's entry for a name: a type, Type.member, or a member
```

<!-- ceesafe: skip -->
```ceesafe
/// A point in the plane.
///   - coordinates are exact integers;
///   - `norm` is the squared length.
public struct Point {
    public x: i64;
    public y: i64;

    /// The squared distance from the origin.
    public fn norm(this) -> i64 => this.x * this.x + this.y * this.y;
}
```

becomes a section with `public struct Point { public x: i64; public y: i64; }`, the list, and a subsection for
`Point.norm`. The [lib/core reference](../spec/api/core.md) on this site is generated this way.
