# ADR 0109 — Private items are private to their file

- Status: **Accepted** by the owner, 2026-11-12 ("We fix all gaps 1-6 first"), implemented on branch `impl/0109`; released in ceesafec
  0.42.0. Raised while writing the language's book: `private fn` at the top level of a file was accepted and
  another file of the package could call it, though `type-system.md` §2 says `private` keeps an item within its file.
- Scope: the checker (`src/check/packages.cee` and the places a name is resolved), one condition of the parser
  (`src/syntax/parser.cee`, an `impl` block's modifiers); `spec/type-system.md` §2 and §12, `spec/grammar.md`,
  `spec/diagnostics.md` (N0296), `spec/tooling.md` §10.1; the guide. No new diagnostic code, no change to the
  libraries, the runtime or the generated C.

## 1. Context

The specification gave `private` one meaning in three places: `language-spec.md` §5 ("visible within the declaring
file"), `type-system.md` §2 ("`private` within its file (§12)") and ADR 0013 D3. ADR 0013 also recorded what was
implemented ("Only `private` on struct fields has an effect"), and §12 repeated it ("the one place where a visibility
modifier takes effect within a package"). So the rule was written and only its field case was checked.

Measured on ceesafec 0.40.0, 2026-11-12, with a library of two files and a program that depends on it (every kind of
item declared four times, once per modifier, and each used once in each place):

| Declared `private` | Same file | Another file of the package | Another package |
|---|---|---|---|
| `fn`, generic `fn`, `struct`, `enum`, `interface`, `const`, `static` | accepted | **accepted** | N0312 |
| a field | accepted | N0296 | N0296 |
| a method or static function in a struct's body | accepted | **accepted** | N0312 |
| an associated constant (struct body or `impl` block) | accepted | **accepted** | N0312 |
| a function in an `impl T { … }` block | **N0100**: not parsed | | |

`internal` and no modifier were the same thing everywhere: accepted in the package, N0312 outside it (for a field,
N0312 too). `public` was accepted everywhere. A function in an `impl T { … }` block was parsed with `public` or with
nothing; `internal fn` and `private fn` there were a syntax error, though `grammar.md` gives every member
`Visibility?`, and `private const` in the same block was parsed.

## 2. Decision

### 2.1 The rule

A `private` item is usable only in the source file that declares it. Naming it in another file of its package is an
error. This holds for

- a function (one of an `extern` block included), struct, enum, interface, constant or static at the top level;
- a method, static function or associated constant, in a struct's body or in an `impl T { … }` block;
- a field, as before (§12).

"Naming" is every place the name is written: a call, a function used as a value, a type, a type argument, a bound,
`dyn I`, an `impl` header, a struct literal, `S.f()`, `E.V`, `S.NAME`, and a pattern (`E.V(x)`, a constant).

`internal`, no modifier and `public` mean what they meant.

### 2.2 Which file

The file that declares the item. For a member of an `impl T { … }` block that is the block's file, not the file of
`T` (ADR 0064 §1 lets a block live elsewhere): a private function there is the block's helper, and the struct's own
file cannot call it. For a field it is the struct's file, which is the same rule.

A generic function's body is its own file's code in every instance: a generic of `a.cee` that uses a private item of
`a.cee` may be instantiated from `b.cee`. The checker already checked a generic's body as its file's (imports and
private fields depend on it).

### 2.3 The diagnostic

N0296, which was "a `private` field named outside the source file that declares its struct". It is the code for
"private to a file", and its meaning is widened; no new code. N0312 stays what it is, "not public" between packages,
and still reports another package's private item (a private field there stays N0296).

```text
error[N0296]: 'helper' is private to src/shapes.cee: only code there can use it (type-system.md §2)
error[N0296]: 'Open.secret' is private to src/shapes.cee: only code there can call it (type-system.md §2)
error[N0296]: 'Open.BASE' is private to src/shapes.cee: only code there can use it (type-system.md §2)
```

A field's message is unchanged. One use is reported once, at the name.

### 2.4 Name lookup: found, and private

A private item of another file is **found and then refused**, not "not found". The message can then say which file
the item belongs to, and the check goes on with the item, so one mistake is one error and not a chain of unknown
names and types.

It follows that the package keeps its one scope (`language-spec.md` §5), and a private item's name is taken in the
whole package:

- two files may **not** each have a `private fn helper`: the second is N0201, as before;
- a private item named like a `lib/core` item hides the library's in every file of the package (ADR 0075 §5), and in
  the other files using that name is N0296, not the library's item.

Giving each file a scope of its own for its private names would allow both. It is the larger change (lookup by file,
and C names that tell two `helper` of one package apart), nothing measured asks for it, and it can be added later
without breaking a program that this ADR accepts: it only turns two errors into accepted programs.

### 2.5 An `impl` block's modifiers

`impl T { … }` takes `internal` and `private` before `fn`, as `grammar.md` said and as its constants already did. The
parser's loop tested for `public` only.

A function of an `impl I for T` block takes `public` or nothing, as the parser always had it; `grammar.md` said
`Visibility?` and is corrected. The modifier has no effect there, since the method is as visible as the interface.
Accepting `private` there would be a modifier that is parsed and not enforced, which is the gap this ADR closes.

### 2.6 What is not checked

- Only names. A value whose type is a private struct or enum may reach another file through a function that is not
  private, and be held, inferred and passed on there; its fields and methods keep their own visibility. There is no
  rule like N0314 for a private type in an internal signature: N0314 protects a package's users from a type they
  cannot name at all, while a file of the same package can ask for the type to be made internal.
- `tests/` files are files: they see the package's internal items, not its private ones. `tooling.md` §10.1 and the
  guide said "private items" where they meant "items that are not public", and are corrected.
- In an `extern` block a private function and a private imported struct are checked, as the items they are. An
  imported C enum (ADR 0079) is a name of another kind in the checker and is not: no test covers a private one.

## 3. Compatibility

This can turn a program that compiled into an error: one that names a private item, method or associated constant
from another file of its package. The fix is in the message: remove `private` (the item is then `internal`, which is
what it was in effect), or move the use into the declaring file.

Measured 2026-11-12, by a search for the declarations (`private` before `fn`, `const`, `struct`, `enum`,
`interface` or `static`, at any indentation, in every `.cee` and `.md` file of the repository) and by running the
new compiler:

| Where | Private items and members declared | Uses from another file |
|---|---|---|
| `src/` (the compiler) | 0 | 0 |
| `lib/` (core, std, kernel) | 0 (67 private fields, checked since C7c) | 0 |
| `tests/reference/` corpus, benchmarks, programs | 3 files, all `parse-pass` and single files | 0 |
| `tests/packages/`, `tests/support/`, `probes/`, `benchmarks/`, `integrations/` | 0 | 0 |
| `guide/` examples | 0 | 0 |

So nothing in the repository had to change for the rule to be turned on, and the compiler's sources needed no first
commit. The corpus (832 files) and the guide's 157 examples pass unchanged. The compiler builds itself with the rule
on; a release before this one does not enforce the rule, and since the sources have no private item, both agree on
them.

`private fn` and `internal fn` in an `impl T { … }` block were a syntax error and are now accepted: no program that
compiled changes.

## 4. Implementation

- `src/check/packages.cee`: `privateFileOf` (the file a private item is confined to), `checkPrivateItem` (N0296 for
  an entity named in another file of its package), `checkPrivateName` (the same for an identifier in an expression,
  unless a local or a type parameter of that name is in scope), `checkMethodPrivate` (called by
  `checkMethodVisible`, so wherever N0312 is checked for a method) and `reportPrivate` (once for each place).
- The calls are where a written name is resolved: a name in an expression, a call's callee, the target of `S.f(…)`
  and `S.x` (`expr.cee`), an assignment's target (`body.cee`), a struct literal (`members.cee`), a named and a
  generic type (`checker.cee`, `options.cee`), an interface in a bound, `dyn` or an `impl` header
  (`interfaces.cee`), a variant path and a constant in a pattern (`patterns.cee`), and `Type.NAME`
  (`member_consts.cee`).
- `src/syntax/parser.cee`: `parseImpl` goes on at any visibility keyword in an inherent block.
- `src/diagnostics/catalog.cee` is regenerated (`tools/gen_catalog.py`) for N0296's meaning.

## 5. Tests

- `tests/packages/private-items` with `tools/package_tests.py` check 31 (the corpus holds single files): three files;
  `src/main.cee` names every kind of private item and member of the other two in every position of §2.1, and the
  check requires N0296 at exactly its 26 marked lines (27 errors) with the declaring file in the message, then
  removes those lines and requires the rest to run and print `31` and `11` (worked out by hand: 12 + 15 + 4, and
  5 + 6): a generic instantiated across files, values of private types, and locals named like private items.
- `tests/reference/tests/corpus/run-pass/private-items.cee`: every kind used in its own file, with `private fn` and
  `internal fn` in an `impl` block; its six lines of output are worked out by hand.
- Run 2026-11-12 on the branch, with the compiler the branch builds (macOS, arm64):
  - `python3 tools/run_corpus.py --compiler out/ceesafec --corpus tests/reference/tests/corpus`: 833 files, 0
    failures (run-pass 328, check-fail 256; the 4 freestanding files partial, as before);
  - `python3 tools/guide_check.py --ceesafec out/ceesafec`: 157 examples (8 skipped), 0 problems;
  - `python3 tools/package_tests.py --compiler out/ceesafec`: 31 checks, 0 problems;
  - `out/ceesafec fmt --check`: no file;
  - the compiler built by 0.40.0, then by itself twice: the second and third generations' `build --emit c` are
    identical (`diff -r`).
- Not run here: `tools/contract_tests.py` (the integrator's), which will ask for `src/library/documents.cee` to be
  regenerated from the changed `spec/` and `guide/`.

## Addendum: an enum's functions (ADR 0107)

ADR 0107, made beside this one, gives an enum functions through `impl E { … }`. They follow §2 as a struct's do: a
`private` function of the block is usable in the block's file only, and N0296 elsewhere in the package. The package
test `private-items` has the case (a method and a static function of an enum in `src/more.cee`).

Where two files each declare a `private` item of one name, the second declaration is N0201 (§2: one scope), and
the uses of the name in that second file are not also reported as N0296: the file means its own item, and one error
says so.
