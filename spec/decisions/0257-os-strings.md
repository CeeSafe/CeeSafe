# ADR 0257 — OS strings: file names, arguments and environment values of any bytes

- Status: **Proposed — implemented** 2026-11-25 on the branch `fix/g5-os` (from 0.95.10), for the SQLite port's
  `CEESAFE_FINDINGS.md` finding 47, at the owner's instruction ("Design an OS-string type, or path type, for file
  names, arguments and environment values, as Rust's `OsStr`/`Path` do"). §7's questions are the owner's.
- Scope: lib/core `osstring.cee` (new); lib/std `host.cee` (every path parameter, `Directory.listOs`,
  `Directory.currentOs`, `Process.argumentOs`, `Process.executableOs`, `Environment.variableOs`); the checker
  (`src/check/references.cee`: an argument for an `OsStringView` parameter; `src/check/io.cee`: the path of the
  `File` built-ins); the C backend and the interpreter (the path of the `File` built-ins); the runtime
  (`runtime/hosted/ceesafe_rt_io.c`: WTF-8 on Windows, raw environment values); type-system.md §22.6 (new), §26.1,
  §26.3, §26.8–§26.10; guide/builtins.md; spec/api/core.md and std.md.

## 1. Context

Every path in CeeSafe is a `StringView`, so it must be UTF-8 (type-system.md §22.2). The systems disagree:

- **POSIX** names files with bytes: anything but `/` and a zero byte. SQLite takes a file name as bytes, and on Linux
  `ATTACH 'a\xffb.db'` makes that file. A CeeSafe program cannot open it, list the directory that holds it
  (`Directory.list` is `InvalidInput`), or receive it as an argument (`Process.argument` is `null`).
- **Windows** names files with UTF-16 units, which need not pair: an unpaired surrogate is a valid name. The runtime
  converted UTF-8 to UTF-16 and back strictly, so such a name could be neither given nor read, and `Directory.list`
  passed over it without a word (the runtime skipped what it could not convert, contrary to §26.10).

Rust's `OsStr` solves both with one representation: bytes on POSIX, and on Windows WTF-8, the generalisation of UTF-8
that also encodes an unpaired surrogate as the three bytes UTF-8 would give its code point. UTF-8 text is the same
bytes in both, so text becomes an OS string with no copy and an OS string that is UTF-8 is text with no copy.

## 2. Decision

### 2.1 The types

lib/core, beside `ByteString` (ADR 0231), written in safe CeeSafe, both `@derive(Equal)` (`==` compares the bytes):

- `OsStringView<region r>`: borrowed and Copy, a private `Slice<r, u8>`; a field of the type needs a region
  parameter (N0270), as a view's does.
- `OsString`: owned, a move type, a private `Vector<u8>`.

They hold **the system's encoding of a name**: on POSIX the bytes the system calls take and give; on Windows the
WTF-8 encoding of the UTF-16 units. The bytes are not checked when an OS string is made: an OS string may hold any
bytes on every system, and a Windows call refuses bytes that are not well-formed WTF-8 with `InvalidInput`, as every
system refuses a zero byte (§2.6). Well-formed WTF-8 is UTF-8 that may also hold the three-byte encoding of a
surrogate (`ED A0 80` to `ED BF BF`), except a lead surrogate's directly followed by a trail surrogate's: a pair is
written as the four bytes of its code point.

**They are lib/core's own types, not `ByteString`s.** A byte string is text whose bytes are unknown; an OS string is
a name for the system, and the type says so where a name goes (`Directory.listOs` gives names, `ByteString`s would not
say whose). They convert to and from byte strings without a copy (§2.2), so the byte-string algorithms (searches,
`chunks`, splitting) serve both.

### 2.2 Making one and converting it

| Call | Copies |
|---|---|
| `OsStringView.fromText(text)` (a literal, a `StringView`, a `String` lent) | no |
| `OsStringView.fromBytes(bytes)`, `OsStringView.fromByteString(v)` | no |
| `OsString.fromString(s)`, `OsString.fromBytes(v: Vector<u8>)`, `OsString.fromByteString(b)` | no: they take the buffer |
| `o.view()`, `o.bytes()`, `v.bytes()`, `v.toByteStringView()` | no: loans |
| `o.intoBytes()`, `o.intoByteString()` | no: they give the buffer |
| `o.intoString()`: `Result<String, OsString>`, the string when the bytes are UTF-8, the OS string back when not | no |
| `OsString.create()`, `fromText(text)`, `copyOf(bytes)`, `o.clone()`, `v.toOsString()` | one allocation, none for no bytes |
| `OsString.fromWide(units: Slice<u16>)`: the WTF-8 of any UTF-16 units | one allocation |

`OsString` appends (`Result<void, AllocError>`, unchanged on failure): `append(name: OsStringView)` (a literal or a
`StringView` too, §2.3), `appendBytes(bytes)`, `appendByte(b)`; `clear()`, `length()`, `isEmpty()`.

**To text, lossless where it can be:**

- `toUtf8()`: `StringView<r>?`, the bytes as text, with no copy, when they are UTF-8; `null` otherwise. `isUtf8()`.
  On Windows a name with an unpaired surrogate is never UTF-8; on POSIX one with an invalid byte is not.
- `toUtf8Lossy()`: `Result<String, AllocError>`, a new string in which each encoded surrogate is one U+FFFD and every
  other ill-formed sequence is replaced as `ByteString.toUtf8Lossy` replaces it (one U+FFFD per maximal subpart, the
  Unicode Standard's §3.9 practice). The same on every system, so a program's output does not depend on where it
  runs; Rust's `to_string_lossy` gives `ED A0 80` three U+FFFD on POSIX and one on Windows.
- `toWide()`: `Result<Vector<u16>?, AllocError>`, the UTF-16 units of well-formed WTF-8, `null` for other bytes;
  `isWtf8()`. With `fromWide` it is Windows' conversion, available on every system so that it can be tested
  everywhere.

There is no implicit conversion from an OS string to text: a program asks for `toUtf8()` (checked, `null` when it is
not text) or `toUtf8Lossy()` (explicit loss).

### 2.3 A `StringView` argument for an `OsStringView` parameter

A **call argument** whose type is `StringView` (a literal, a view) or `String` (lent, as for a `StringView`
parameter) is accepted for a parameter whose type is lib/core's `OsStringView`: the checker passes
`OsStringView.fromText(argument)`, which copies nothing, and the view lives as the argument's text does. This is
what keeps every existing call compiling when a parameter changes from `StringView` to `OsStringView`
(`Directory.create("out")`, `File.rename(from, to)` with `String`s lent), and it serves a program's own functions
too. Only arguments: `let p: OsStringView = "a";` is still N0205, whose fix is `OsStringView.fromText("a")`, and a
`Vector<StringView>` is not a `Vector<OsStringView>` (the process calls take either, ADR 0258 §2.4).

ADR 0231 §2.7 declined a literal as a `ByteStringView`: it would have been the first conversion of a built-in value
into a library struct, for a saving of one call. Here the saving is every path argument in every existing program,
and the conversion is a library call the checker writes, which the interpreter and the C backend see as any call.

### 2.4 What takes an OS string

Every name given to the system: a `StringView` there stays valid (§2.3).

- The `File` built-ins `open`, `create`, `remove`, `openReadWrite`, `openOrCreate`, `createNew`, `openAppend`,
  `openRegular`, `createReadWrite`: their path is a `StringView` or an `OsStringView` (the checker passes a view's
  bytes, so a text path compiles as before).
- lib/std: `File.kind`, `File.rename`, `File.modified`, `File.setPermissions`; `Directory.create`, `remove`, `space`,
  `sync`, `list`, `listOs`, `appData`; the program of `Process.capture`, `spawn`, `spawnWith`, and of ADR 0258's
  `runWith` and `captureWith`, and their arguments as a `Vector<OsStringView>` (ADR 0258 §2.4); the names and values
  of a child's environment (ADR 0258).
- `Environment.variableOs(name: OsStringView)`.
- `Process.run`, a built-in, keeps `StringView`s; `Process.runWith` (ADR 0258) takes OS strings.

### 2.5 What gives an OS string

The `StringView`/`String` forms stay, each refusing what is not text as before; beside them:

| Text (refuses a name that is not UTF-8) | OS string |
|---|---|
| `Process.argument(i)`: `StringView?`, `null` | `Process.argumentOs(i)`: `OsStringView<static>?`, `null` only past the end |
| `Environment.variable(name)`: `StringView?`, `null` | `Environment.variableOs(name)`: `OsStringView<static>?`, `null` only when unset |
| `Directory.list(path)`: `Vector<String>`, `InvalidInput` | `Directory.listOs(path)`: `Result<Vector<OsString>, IoError>`, sorted by byte value |
| `Directory.current()`: `String`, `InvalidInput` | `Directory.currentOs()`: `Result<OsString, IoError>` |
| `Process.executable()`: `String`, `InvalidInput` | `Process.executableOs()`: `Result<OsString, IoError>` |

### 2.6 The runtime

- **POSIX.** The runtime already took and gave bytes: the paths were checked as text in lib/std and the checker
  only. `ceesafe_host_env` no longer drops a value that is not UTF-8 (lib/std's `Environment.variable` checks), and
  `ceesafe_host_argument` gives an argument's bytes. A zero byte or more than 4,095 bytes is still `InvalidInput`.
- **Windows.** `cee_win_wide` (every path, the command line, the environment) decodes WTF-8 instead of UTF-8, and
  `cee_win_narrow` (a directory's names, the working directory, the executable's path, the arguments, the
  environment) encodes WTF-8, so no name is lost either way. Bytes that are not well-formed WTF-8 are `InvalidInput`
  (`EINVAL`). The environment is read as UTF-16 (`GetEnvironmentStringsW`, once, kept for the process's life, names
  compared without case as Windows does), not the C runtime's ANSI copy, so a variable with non-ASCII text is no
  longer lost. `Directory.list` now refuses a name that is not UTF-8 there, as §26.10 always said, instead of
  passing over it.
- The arguments on Windows are ADR 0259's.

### 2.7 The interpreter

`ceesafec run --interp` runs a program's file calls through the compiler's own, which the release's stage 0 (0.95.10)
must still build, so its `File` takes text: the interpreter refuses a name that is not UTF-8 with `InvalidInput` (it
panicked before, when the checker could not let one through), gives `Directory.listOs` only names that are UTF-8
(else `InvalidInput`), and `Environment.variableOs` and `Process.argumentOs` only values that are. Once a stage 0
has OS strings, the interpreter can pass them through.

## 3. Alternatives

- **An interface, `fn open<P: PathName>(path: P)`.** A `StringView` implements no interface and a type parameter
  takes no scoped type (N0267, N0274), so literals would not be accepted.
- **`ByteStringView` as the path type.** It names no encoding, so a Windows program could not tell what its bytes
  mean, and a name read from the system would come back as a type that also holds arbitrary text.
- **UTF-16 on Windows (`Vector<u16>`).** Every name would differ by platform in the program, and no name would be
  text without a conversion; WTF-8 keeps UTF-8 names as they are and costs a conversion only at the system call,
  where one was made already.
- **A `Path` type with components.** Path manipulation (joining, parents, extensions) is a library above this one; a
  name type comes first, and `OsString.append` builds paths meanwhile.

## 4. Migration

- Every call that compiled compiles unchanged and does what it did: a `StringView` or `String` argument for a path is
  converted (§2.3), and the text-giving calls keep their types.
- lib/std's signatures change from `StringView` to `OsStringView` (spec/api/std.md). A program that **takes the
  address** of one of them as a function value of a `StringView` parameter (`let f = Directory.create;` used as
  `fn(StringView) -> …`) no longer compiles; none does in the compiler, lib/, CeeSafeLibs or CeeSafeApps.
- The compiler's own sources (`src/`) keep their `StringView` paths: they are built by stage 0 0.95.10, which has no
  OS strings, and they compile unchanged under this tree with no warning.
- On Windows, `Directory.list` refuses a directory that holds a name that is not UTF-8 where it skipped the name
  (use `listOs`).

## 5. Tests

- `tests/reference/tests/corpus/run-pass/adr0257-os-strings.cee`: every member once, the coercion for literals,
  views, `String`s, a program's own `OsStringView` parameter and a method's; WTF-8 cases from
  `tools/oracles/gen_os_string_cases.py` (Python's `surrogatepass` codec): `fromWide` and `toWide` of lone and paired
  surrogates, ill-formed WTF-8 (`ED A0 80 ED B0 80`, `ED` truncated), and `toUtf8Lossy` of each; natively,
  `--interp` and `--check-leaks`.
- `tools/process_check.py` (contract step `processes`, and windows.yml): on Linux and FreeBSD a file named
  `a\xffb.txt` is made, opened, listed by `listOs`, renamed and removed by a CeeSafe program, and passed to it as an
  argument, compared with what Python's `os` sees; on macOS the same names are refused by the file system
  (`EILSEQ`, `InvalidInput`), as Python's `open` is, and the check says so; on Windows a name with a lone surrogate
  (`a\udc80b.txt`, made by Python) is listed, opened, renamed and removed, and passed as an argument.

## 6. Consequences

- A CeeSafe program can name every file its system can, and receive every argument and environment value.
- lib/core has two more public types (spec/api/core.md); emit-c snapshots that print type ids may move.
- The `File` built-ins' C takes `cee_str` as before: an OS string's bytes are passed as one.

## 7. Questions for the owner

1. The OS-string forms are named with an `Os` suffix (`listOs`, `currentOs`, `argumentOs`, `variableOs`,
   `executableOs`), as Rust's `args_os` and `var_os`. Should a later release make the OS forms the plain names?
   **Answered 2026-11-25: keep the `…Os` names for now**; making the OS forms the plain names is for 1.0.
2. `toUtf8Lossy` replaces an encoded surrogate by one U+FFFD on every system (Rust: on Windows only). Keep it uniform?
   **Answered 2026-11-25: keep it uniform**, one U+FFFD per encoded surrogate on every system.
3. `Process.run`, a built-in, keeps `StringView`s; `runWith` takes OS strings. Should `run` become lib/std's too?
   **Answered 2026-11-25: `Process.run` stays a built-in for now**; moving it to lib/std is a later item.
