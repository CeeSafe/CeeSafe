# ADR 0230 — Each file's private items have a scope of their own

- Status: **Proposed** 2026-11-24, under the owner's instruction "Implement fixes for findings 6, 15, 19, 26 and 35 in
  CeeSafe" (`CeeSafeApps/sqlite/CEESAFE_FINDINGS.md` finding 15). Implemented 2026-11-24 on `fix/findings2-names`.
- Scope: name lookup in the checker (`src/check/declare.cee`, `packages.cee`, `inherent.cee`, `suggest.cee`, the
  places a type item is declared), C names (`src/backend/c_names.cee`, `c_skeleton.cee`); `spec/type-system.md` §2,
  `spec/language-spec.md` §5, `spec/c-backend.md` §2.1, `spec/diagnostics.md` (N0201, N0296); the guide
  (`guide/packages.md`). Amends ADR 0109 §2.4, which left this step for later.

## 1. Context

**Finding 15.** Two files of the SQLite port written apart, `datetime.cee` and `printf.cee`, each defined helpers
`appendInteger` and `appendFloat`; merged into one package they stopped compiling with N0201. The port's workaround
is to prefix helpers with their file's subject (`printfInteger`).

The language already has file-private items: ADR 0109 made `private fn`, `private struct`, … at the top level usable
only in their file (N0296 elsewhere). It kept one scope for the package, so a private item's name was still taken in
every file, and said (§2.4) that giving each file a scope of its own for its private names "can be added later
without breaking a program that this ADR accepts: it only turns two errors into accepted programs". The finding is
the program that asks for it. So no new syntax: `private` at the top level is the form, and what changes is that a
private name no longer occupies the package's scope.

Measured with ceesafec 0.95.0 before the change: two files each with `private fn helper()` is N0201 at the second;
a private item named like a lib/core item (`private struct Layout`) hides lib/core's `Layout` in every file of the
package, where naming it is N0296.

## 2. Decision

### 2.1 Two scopes a file sees

Each file of a package sees two scopes of items: the **package's** (its `public` and `internal` items, one scope for
all its files, as before) and the **file's own** (its `private` top-level items: functions, generic functions,
structs, enums, interfaces, constants, statics, and the functions of an `extern` block marked `private`). A name in a
file means, in this order:

1. a local, a parameter or a type parameter (unchanged);
2. the file's own private item;
3. the package's item;
4. `dep.X`, then the file's imports (unchanged);
5. lib/core's item, then a built-in function (unchanged);
6. else a private item of another file of the package: found and refused, N0296, as ADR 0109 §2.4 has it.

So two files may each declare a private item of one name, and each means its own.

### 2.2 A private item and a package item of one name

A file's private item may have the name of an item of the package declared in another file; inside its file the
private one is meant (step 2 before step 3), everywhere else the package's. Within one file, two items of one name are
N0201 whether they are private or not, as before.

The alternative was an error (N0201) for a private name that is also a package name. It was not taken because it
recreates the finding one level up: the port's two files would compile only if both had made their helpers private;
an `internal` helper added later to any file (or a teammate's file that never used `private`) would break every
file that has a private helper of that name, though none of them can see the other's. A file-private scope is useful
exactly because what one file declares privately does not depend on what the rest of the package declares. The
shadowing is visible where it happens: the private declaration is in the same file as every use it captures. It is
the rule a local already follows against an item (a local `helper` in `main` means the local, ADR 0109's test).

The same holds for lib/core: a private item named like a lib/core item (`private struct Layout`) hides the library's
in its own file only; other files of the package get lib/core's (ADR 0109 said every file, with N0296 in the
others). A built-in function's name (`truncate`) is found before another file's private item too.

### 2.3 What is unchanged

- Members: a private method, static function or associated constant (ADR 0109) belongs to its type, which has one
  set of member names; nothing changes. Methods of two private types of one name are the methods of two types.
- An `impl` names its interface and type as its file means them: `impl Shape for Row` in `printf.cee` implements
  `printf.cee`'s private `Shape` for `printf.cee`'s `Row`, and `impl Mode { … }` adds functions to the file's own
  private `Mode` (the inherent block's target is looked up in the same order, §2.1).
- A generic function's body is its file's code in every instance (ADR 0109 §2.2): a private generic of `a.cee`
  instantiated through a non-private function of `a.cee` from `b.cee` still means `a.cee`'s names.
- Another package sees none of a package's private items: `dep.X` or `import dep.X` of a private `X` is N0312 as
  before; and a dependency's public `X` stays visible though one of its files also has a private `X`.
- The entry function (`main`, or the name `--entry` gives) keeps one scope in the package: a `private fn main` is the
  package's `main`, and a second `main` in another file is N0201 whatever its visibility. C calls it by its name.
- C names that C itself defines (functions of `extern "C"` blocks, `@export`, `@repr` types) stay one namespace, and
  two declarations of one C name are N0201 as before (`type-system.md` §28.2, ADR 0081 §2.2): `private` on such an
  item limits who may name it, not the C symbol.
- Libraries (`lib/`) have one scope, as before; they declare no private top-level item.

### 2.4 Diagnostics

No new code. N0201's meaning narrows: "defined twice in the package" becomes "defined twice in one scope: the
package's items, or one file's private items and the package's items of that file". N0296 is unchanged: naming
another file's private item when nothing else of that name is visible (§2.1 step 6) is "'helper' is private to
src/printf.cee: only code there can use it". The "did you mean" of an unknown type name offers only the types the
file can name, not another file's private ones.

### 2.5 C names

A private top-level item's C name carries its file: `00<n>_` before the name, where `n` is the file's number in the
build: `ceeF_003_appendInteger`, `ceeT_004_Row`, `ceeM_004_3Row_value`, `ceeG_003_5twice1i64`, a mangled
`S004_3Row`, `ceeD_004_3Row`, a private interface's `dyn` code `D004_5Shape`, `ceeS_003_TABLE`. No other name starts
`00`: a dependency's item starts with its package name's length, which is never `0`, and in a name written with its
length a dependency's starts with a single `0` then a length (c-backend.md §2.1). The mark is written for every
private item, not only for names that meet, so that the C name of an item does not depend on the other files.

The file's number is its index in the build, which is the same in every build of the same files. It is not part of
any interface record (those are made by `check`, which emits no C).

## 3. Options not taken

- **N0201 for a private name that a package item also has** (§2.2).
- **A module or namespace per file** (`printf.appendInteger`): a larger change to naming, imports and the package
  model for what `private` already expresses.
- **Marking only private names that meet another**: the C names of one file would change when another file adds an
  item, so builds of one file would not be comparable; the mark is short.

## 4. Compatibility

Every program ADR 0109 accepted is accepted, with the same meaning, except one that used, in one file, a lib/core item
whose name another file of the package declares `private`: that was N0296, and now means lib/core's item. Programs
that ADR 0109 refused with N0201 (two files with a private item of one name; a private and an internal item of one
name in different files) are accepted. C names of private items change (the mark); nothing outside a build refers to
them.

Measured 2026-11-24 by a search for top-level `private` declarations (`private` before `fn`, `struct`, `enum`,
`interface`, `const`, `static` at the start of a line): `src/` (the compiler) and `lib/` have none, so the compiler's
own C is unchanged and its fixed point holds; the corpus has three files (`run-pass/private-items.cee`,
`parse-pass/visibility.cee`, `parse-pass/statics.cee`), single files, unchanged in meaning; `tests/packages/private-items`
has `private fn min` in `shapes.cee` and calls `min(1, 2)` in `main.cee`: neither lib/core nor the built-ins have a
`min`, so that is still N0296 and the test is unchanged; the book has two private functions, each used in its own
file only, and its chapter 7 text that said two files cannot each have a private function of one name is
corrected.

## 5. Implementation and tests

- `src/check/declare.cee`: `ownItem` puts a private item of a package's file in `Checker.fileItems` (by file and
  name), and the first of each name in `Checker.privateItems` (for N0296); `isDeclaredAs` is N0201's test for each
  kind of declaration (§2.2); `entityFileOf`, `isEntryName`.
- `src/check/packages.cee`: `packageItem` looks up in the order of §2.1; `isNameableHere`; `declarePackageName`
  records a file's private names apart (`Checker.fileDeclared`), so a dependency's private name does not hide its
  public one of the same name, and an import is ambiguous (N0313) unless the file itself sees the name
  (`declaredForFile`).
- `src/check/inherent.cee`: `typeItemNamed` finds an `impl T { … }` block's struct or enum as the block's file means
  it. `src/check/suggest.cee`: `addCloseTypeName` leaves out another file's private types.
- `src/backend/c_names.cee`: `appendPrivateMark`, written by `appendShadowMark` (types), `appendCFunctionName`
  (functions, generic instances, extern wrappers), the drop glue's name, `appendInterfaceMark` (`dyn` codes);
  `src/backend/c_skeleton.cee`: `appendStaticName`. The drop glue's name had no mark of any kind, so it now also
  takes `appendShadowMark`'s `lib_` for a library type a package's item shadows (ADR 0075 §5), which another type
  of that name's drop glue would have met.
- Tooling: `ceesafec doc` documents public items only, so no private item, as before. The language server's
  definitions come from what the checker resolved, so they follow §2.1 with no change; it offers no completion.
- `tests/packages/file-private-names`, `tools/package_tests.py` check 46 (the corpus holds single files): three files,
  `printf.cee` and `datetime.cee` each with private `appendInteger`, a generic `twice`, a struct `Row` with a method
  and a `Shape` impl, an enum `Mode` with an `impl Mode` block, an interface `Shape` used as `dyn`, a constant, a
  static; `printf.cee`'s private `Layout` and `label` hide lib/core's `Layout` and `main.cee`'s `label` in that file
  only. The program prints `1148` and `74` (worked out by hand in `main.cee`'s comment) in the interpreter and
  compiled, and the C defines two `ceeF_00<n>_appendInteger`; a fourth file that calls `appendInteger` is N0296
  naming the file, its unknown type `Rwo` gets no "did you mean 'Row'", and the language server's definition of
  `appendInteger` called in `datetime.cee` is `datetime.cee`'s.
- `tests/packages/private-items` (check 31) passes unchanged.
- `check-fail/adr0230-file-private-names.cee`: in one file, a private and an internal item of one name are N0201, as
  two private ones are.
