# ADR 0073 — Temporaries as receivers, two-phase `mut` arguments, block values in branches, and a first warning

- Status: **Accepted** by the owner, 2026-11-05 ("Accept the ADR as proposed": §6 as proposed: temporaries for
  `borrow this` and `borrow mut this` receivers, two-phase reservations allow reads only, `--deny-warnings` on
  `check`, `build` and `test`, the B5 order fix in this release with a release note, one release as 0.17.0). Implemented with the changes listed in §7. Raised by the mail server port on 0.15 (`CeeSafeApps/mailserver/CEESAFE_FINDINGS.md`
  §2.2, §3.2, §3.3, §3.6; the running log is `CeeSafeLibs/NOTES-ceesafe-0.14.md`), passed on by the owner ("Draft the
  ADR for §3.2, §3.3, §3.6 and §2.2"). Each finding was reproduced with ceesafec 0.16.1 on 2026-11-05.
- Scope: type-system.md §13.2 (creating references), §15.3 (`if` and `match` expressions), §16.2 (slicing),
  §14.5 (destruction of temporaries); lifetimes.md §5 (B5) and §6 (temporaries); ownership.md B5; language-spec.md
  (evaluation order); grammar.md §1.6 (escapes) and the `Branch` rule; diagnostics.md (a new warnings range);
  tooling.md (warnings in the human, JSON, LSP and MCP output); the checker, NIR lowering, the interpreter, `fmt`;
  guide/for-agents.md, templates/AGENTS.md; the corpus.

## 1. Context

The mail server port is about 25,000 lines across PostgresClient, tls, the crypto packages and eight mail packages.
After 0.16.0 and 0.16.1, four of its open findings come up most often while writing code. Three of them cost a line
or a helper each time. The fourth produces a wrong program without a word.

- **A temporary cannot be a receiver** (§3.2). Passing a temporary to a `borrow` parameter already works
  (§13.2: "a value becomes a temporary that lives until the end of the statement"), so `useb(make())` compiles. But
  `make().view()` is N0247 ("only a variable, a field, or a place reached through a reference can be borrowed"), and
  so is `use(make().view())`. The receiver of a `borrow this` method has to be a place. The same rule refuses
  `make()[0..1]` (§16.2) and `table().length` (N0216). The notes say it "happens constantly in tests and string
  building", and every case is the same two lines: `let tmp = make(); use(tmp.view());`.
- **No two-phase borrows for `mut` arguments** (§3.3). B5 (lifetimes.md §5) lets a `borrow mut this` method's
  arguments read the receiver (`v.push(v.length)`), but a free function's `mut` argument gets no such allowance:
  `fail(mut s, s.count + 1)` is N0242 ("cannot use 's.count' while 's' is mutably borrowed"), although `s.count` is
  a `u32` copied before the call. msieve carries a separate `Problem` struct to work around it.
- **A `match` or `if` branch can't be a block that gives a value** (§3.6). §15.3 allows a block branch only if it
  cannot complete, and says "a block that ends in a value, as in Rust, is not part of G5". So
  `Some(v) => { let w = v + 1; w }` is N0100. The workaround is a `var` assigned in every arm, or a helper function.
- **`"x = {x}"` prints `x = {x}`** (§2.2). CeeSafe has no string interpolation. C# and TypeScript habits write
  it anyway, and nothing says so. It is the only one of the four that produces a wrong program rather than an error.

Reproducing §3.3 turned up a bug in B5 as implemented. `nir_builder.cee` lowers the other arguments first and the
receiver's place last, so an argument that changes an index in the receiver's place changes which element is
borrowed:

```ceesafe
var ps = [P { v: 0 }, P { v: 0 }];
var i: usize = 0;
ps[i].set(bump(mut i));   // bump: i += 1, returns 9. Writes ps[1]: the receiver's index is read after the argument.
```

language-spec.md requires "strictly left to right, receiver before arguments", so `ps[0]` is the right answer.
lifetimes.md §5 justifies B5 with "evaluating a place has no side effects, so creating its loan later does not change
what the program computes". That holds for the loan, but not for the place's index operands, which are evaluated
with it.

## 2. Proposal

### 2.1 A temporary as a receiver, a slice base or a `.length` operand (§13.2, §16.2)

A **receiver** of a `borrow this` or `borrow mut this` method that is not a place is evaluated into a temporary, and
the receiver borrows the temporary. So is the **base of a slice** (`e[lo..hi]`), and the **operand of `.length`** on
an array. It is the rule §13.2 already applies to a value passed to a `borrow T` parameter:

```ceesafe
use(make().view());                          // the String lives until the end of the statement
let n = make().view().length;                // a usize, copied out: nothing borrows the temporary afterwards
let first = text.split(",").next();          // a borrow mut this method on a temporary TextSplit
let head = digest(data)[0..4];               // a slice of a temporary [u8; 32]
let count = table().length;                  // an array result: its length is N; the call still runs
```

- **Lifetime.** The temporary lives until the end of its statement (§14.5). In an `if` or `while` condition it lives
  until the condition has been evaluated, and in a branch of an `if` or `match` expression until the branch ends
  (§15.3), exactly as today's temporaries do. It is destroyed then, `drop` included.
- **A result that borrows it.** A result derived from the temporary (`make().view()`) can be used inside the
  statement. Kept in a `let` that is used after the statement, it is N0243 ("temporary dropped while still
  borrowed"), the error `same(five())` already gives, with a note naming the temporary and suggesting a local.
- **Field paths.** A field path on a temporary is a place of that temporary: `make().name.view()` borrows the
  temporary's `name`.
- **`borrow mut this` receivers** are included. The receiver has no marker at the call site, and the common case
  is an iterator or a builder made for that one call (`text.split(",").next()`). **(Question 1.)**
- **Unchanged:**
  - An explicit `borrow e` or `mut e` of a non-place is still N0247 (`borrow 5`, `take(mut five())`). The `mut`
    marker says the caller will see the change, and a temporary's change can't be seen. An explicit `borrow` of a
    value adds nothing: pass the value, or bind it.
  - N0244 is unchanged: a function cannot return a reference to its temporary.

### 2.2 Two-phase `mut` arguments, and B5 evaluated left to right (lifetimes.md §5, ownership.md B5)

**Every** argument written `mut p`, with `p` a place, and every `borrow mut` reference local passed on to a `borrow
mut` parameter (a reborrow, B4), is **reserved** where it stands and **activated** at the call. Today that is true
only of a `borrow mut this` receiver (B5). The rule covers functions, static functions, methods' other arguments,
closure and `dyn fn` calls alike:

```ceesafe
fail(mut s, s.count + 1);           // was N0242
record(mut log, log.entries.length); // was N0242
```

- **Evaluation stays left to right.** The place's own operands, the index expressions in `mut a[i].f`, are evaluated
  where the argument stands. The bounds check happens there too, and the values go into temporaries. Only the
  creation of the loan moves to the call. B5's receivers follow the same rule. This fixes `ps[i].set(bump(mut i))`
  (§1), which now writes `ps[0]`, as language-spec.md requires.
- **While reserved, the place can only be read.** Between its argument and the call, the reserved place behaves as
  if it were borrowed shared:
  - **Allowed:** a read of an overlapping place, a copy (`s.count`) or a shared borrow that ends before the call
    (`s.name.view().length`).
  - **N0241**, with a note "reserved here for the `mut` argument (l:c)":
    - a shared borrow still live at the call, such as `f(mut s, s.name.view())` or `f(mut s, borrow s.inner)`;
    - a `borrow mut` of an overlapping place, such as `f(mut s, g(mut s))`;
    - a second `mut` argument for an overlapping place.
  - **N0240:** a write to an overlapping place.
  - **N0251:** a move out of one, as for any use of a borrowed place.
- **Why it is sound.** No mutable loan exists until the call. Each read finishes, or its shared loan ends, before
  the loan is created. The place denotes the same storage at the call as at the argument: its index values were
  fixed at the argument, and while it is reserved, nothing can write to it or to any reference it passes through.
- **Not covered.** A *mutable* use of the place in a later argument stays an error. That includes PostgresClient's
  `run(mut connection, await connection.batch(sql))`, which the notes list beside §3.3. Write
  `let rows = await connection.batch(sql); run(mut connection, rows);`. **(Question 2.)**

### 2.3 A block branch may end in its value (§15.3)

A block branch of an `if` or `match` **expression** may end with an expression written without `;`. That expression
is the branch's value:

```ceesafe
let x = match (y) {
    Some(v) => {
        let w = v + 1;
        w
    }
    null => 0,
};
let label = if (n == 1) "one" else {
    let rest = describe(n);
    rest.kind
};
```

```text
Branch     = Expr | Block | ValueBlock
ValueBlock = '{' Statement* Expr '}'       (* the final Expr has no ';' *)
```

- **Where.** Only in the branches of `if` and `match` *expressions* (§15.3). Function bodies, the statement forms of
  `if` and `match`, and plain blocks keep their meaning. A function returns with `return` or `=>`, as now. In a
  statement-form `match`, a final expression without `;` is still N0100. Its hint now says that a statement-form
  `match` has no value, and that `let x = match …` makes it an expression.
- **The final expression.**
  - **Typing.** It is the branch's expression for typing: it is checked against the expected type, or it fixes the
    type of the whole, exactly as an expression branch does today.
  - **`if` or `match` at the end.** One that ends a value block, directly before `}`, is read as an expression.
    `{ let w = f(); if (w > 0) 1 else 2 }` works. Today a branch block that ends in a statement-form `if` must
    already be unable to complete, and read as an expression it still can't, so existing programs keep their
    meaning.
  - **A block that can complete with no final expression** is still N0235. Its hint now says "end the block with its
    value, written without `;`".
- **Ownership.**
  - The value is moved (or copied) out of the block before the block's locals and temporaries are destroyed.
  - A reference to a block local or temporary cannot be the value: N0243, as for an expression branch's temporary
    (§15.3).
  - A `return`, `break` or `continue` before the final expression is allowed, as in any block.
- **Formatting.** `ceesafec fmt` writes a value block's statements and its value one per line, the value last, and
  no comma after the block (§15.3: a block arm needs none).

### 2.4 The first warning: `{name}` in a string literal (N0400)

```text
main.cee:3:23: warning[N0400]: this string prints '{x}' as written: CeeSafe has no string interpolation
  |
3 |     Console.writeLine("x = {x}");
  |                       ^^^^^^^^^
  = help: write the text and the value in two calls: Console.write("x = "); Console.writeLine(x);
  = help: to mean the braces, write "x = \{x}"
```

- **When.** A string literal's source text contains `{` *name* (`.` *name*)* `}`, the first *name* is bound where the
  literal stands (a local, a parameter, `this`, or a constant), and the `{` is not escaped.
  - `\u{…}` escapes are not candidates.
  - A name that is not bound is not a candidate. `"^{commit}"` in `src/packages/workspace.cee`, a git revision
    suffix, doesn't warn.
  - A search on 2026-11-05 across `CeeSafe`, `CeeSafeLibs` and `CeeSafeApps` found 18 matches of `{name}` inside
    double quotes:
    - `\u{…}` escapes;
    - `${NAME}` and `{x}` in the embedded documentation (`src/library/documents.cee`);
    - `"^{commit}"`;
    - examples inside comments.

    None of them names a binding where it stands, so no existing code should warn. The implementation confirms
    this by running over the three repositories (§5).
- **`\{` and `\}`, two new escapes** (grammar.md §1.6), mean the characters `{` and `}`. Writing `\{` is how a
  program says it means the braces, and it silences N0400. No other way to silence it is added.
- **Warnings in general.** This is the first warning the compiler emits. diagnostics.md gains a range for them,
  N0400–N0499 ("Warnings"), and these rules:
  - **Exit status.** A warning doesn't stop `check`, `build`, `run`, `test` or `fix`, and doesn't change the exit
    status. **(Question 3.)**
  - **Output.**
    - In the human format, a warning is written `warning[N0400]`.
    - In the JSON document (tooling.md), it has `"severity": "warning"` and is counted in `"warnings"`; the field
      already exists and is always 0 today.
    - The language server sends it with severity Warning.
    - MCP `ceesafe_check` lists it.
  - **Which files.** Warnings are reported only for the root package's files, not for dependencies, lib/core or
    lib/std, so a library's warnings never fall on its users.
  - **Checklists.** templates/AGENTS.md and the guide's checklist say "`ceesafec check` reports no errors and no
    warnings".
- **Not an error.** Text with literal braces is legitimate: route templates, git revision syntax, documentation. An
  error would break programs that are correct. A warning that fires only when the name is in scope catches the
  mistake with no false alarms in the code we have.

### 2.5 Guide and template changes

- **guide/for-agents.md:**
  - "A temporary is not sliced or measured" (which 0.16.0 rewrote to `table().length`) is removed. Its example now
    compiles.
  - A new pitfall shows `"x = {x}"` and N0400.
  - The "Arrays of structs" example is unchanged.
- **templates/AGENTS.md** ("Common mistakes"):
  - "Bind a constant or a temporary to a local before slicing it or asking its `.length`" becomes "A view of a
    temporary lasts until the end of the statement: bind the value to a local to keep the view".
  - A line says that there is no string interpolation.
- **guide/structs-and-enums.md**, at `if` and `match` expressions: value blocks.

## 3. Alternatives considered

- **Extend a temporary's life to the enclosing block when a `let` keeps a view of it**, as Rust does for `let r =
  &make();`. This would make `let v = make().view(); use(v);` compile. Rust limits the rule to syntactic cases,
  and it surprises people at the edges: `let r = make().view();` is extended, `let r = id(make().view());` is not.
  CeeSafe's rule stays one sentence: a temporary ends with its statement. The notes' cases all finish in one
  statement.
- **Two-phase for every use that ends before the call, mutable ones included** (§2.2, Question 2). This would
  accept `run(mut connection, await connection.batch(sql))`. It is sound once index values are fixed at the
  argument, but `f(mut s, g(mut s))` then runs `g`'s change to `s` before `f` sees `s`. In a C#-like language,
  readers expect the arguments to be independent there. Rust refuses it too. If the libraries keep meeting it, this
  can be widened later without breaking anything.
- **Block values everywhere** (a function body or any block ending in an expression, as in Rust). CeeSafe already
  has `=> expr` bodies and explicit `return`. A body whose value is its last line would be a second way to return,
  and a missing `;` would change what a function returns. Keeping it to branches, where §15.3 already makes blocks
  special, gives the notes' case and nothing else.
- **A `yield` or `=>` keyword for a block's value** (`{ let w = v + 1; yield w; }`). It's explicit, but `yield`
  is reserved for generators (grammar.md §1.3), and `=> w` inside a block reads like an arm. The final-expression
  form is what C#'s switch-expression users and Rust users already expect.
- **String interpolation** (`$"x = {x}"`). It is the real answer to §2.2, but it needs a design for where the text
  goes (a `TextWriter`, an allocating `String`), for formats (`{x:x8}`) and for scoped values. That is an ADR of its
  own. N0400 is right with or without it: an interpolated string would carry a prefix, and a plain literal would
  still not interpolate.
- **Making N0400 an error.** See §2.4, "Not an error".

## 4. Not in this ADR

These are the mail server's other open findings. Each needs its own ADR:

- generics over `void` (§3.7);
- async children and `Async.timeout` that move their captures out (§3.9);
- region parameters on methods, a method result tied to the struct's region only, and `var` views (§3.10);
- multi-line or raw string literals, `embed("file")`, and constants in structs (§3.11);
- an optional field passed as an optional borrow (§3.12);
- `for` over a type with `next() -> T?` (§3.13);
- `as` from `usize` to `i64` where the value range allows it (§3.14);
- a local shadowing a dependency's name (§4);
- a never-returning type for functions such as `die()` (§4.1);
- string interpolation (§3 above).

## 5. Testing

- **Temporaries (§2.1):**
  - run-pass programs for:
    - a `borrow this` method on a call result, a field of one, and a chained call;
    - a `borrow mut this` method on a temporary iterator;
    - slicing a temporary vector or array;
    - `.length` of an array result;
    - a temporary with a `drop` block destroyed at the end of the statement, its output showing the order;
    - the same inside a condition and inside a branch;

    each also in the interpreter, and with `--check-leaks`.
  - check-fail programs for:
    - a view of a temporary kept in a `let` and used after the statement (N0243);
    - `borrow five()` and `take(mut five())` (still N0247).
  - `check-fail/arrays.cee` (`make()[0..1]`) and `check-fail/region-types.cee` (`Scratch.enter().alloc(1)`) expect
    N0247 today. They become N0243, with a use after the statement, or move to run-pass.
- **Two-phase (§2.2):**
  - run-pass:
    - the notes' `fail(mut s, s.n[0])`;
    - a `mut` argument followed by a read of the same place through a field;
    - a reborrowed `borrow mut` local;
    - `ps[i].set(bump(mut i))` printing `9` then `0` (it prints `0` then `9` today); compiled and interpreted.
  - check-fail:
    - a shared borrow live at the call;
    - a second `mut` of an overlapping place;
    - a write, a move and a `borrow mut` during the reservation (N0241, N0240, N0251).
  - The mutation suite and the fault and cancellation sweeps run unchanged, and the fixed point checks that the
    compiler's own code (which uses B5 heavily) compiles to the same behaviour.
- **Block values (§2.3):**
  - run-pass: value blocks in `match` and `if` expressions; a value block whose last item is an `if` expression; a
    non-Copy value moved out of a block; a `return` inside a value block. Each also in the interpreter.
  - parse-fail: a final expression without `;` in a statement-form `match` (N0100, the new hint).
  - check-fail: a reference to a block local as the value (N0243); a completing block with no final expression
    (N0235, the new hint). `check-fail/branch-expressions.cee` keeps its N0235 line.
  - `fmt`: snapshots of value blocks.
- **N0400 (§2.4):**
  - a check program with every candidate shape:
    - bound and unbound names;
    - `{a.b}`;
    - `\{x}` and `\u{41}`;
    - a constant;
    - a literal outside `Console.write`;

    with the expected warnings. `run_corpus.py` learns `// warning[N0400]` expectations, and a warning-only program
    must still run.
  - The JSON, LSP and MCP outputs: one test each, through `tools/test_command_check.py` and `mcp_api_check.py`.
  - A dependency's warning isn't reported: a package test in `tools/package_tests.py`.
  - The compiler, CeeSafeLibs and CeeSafeApps produce no N0400. This checks the binding rule against real code.
- **The libraries:** after the release, msieve's `Problem` struct, the `let tmp = …` lines the notes count, and the
  `var`-per-arm matches are simplified, and their tests rerun. This is a check of the design, not a gate.

## 6. Questions for the owner

1. **Temporaries for `borrow mut this` receivers** (`text.split(",").next()`), or only for `borrow this`? The risk
   is `make().append("x")!`, a change made to a value that is then destroyed. **Proposed: both.** The receiver has
   no `mut` marker that promises the caller will see a change, and the iterator case is common.
2. **Two-phase: reads only, or any use that ends before the call?** Reads only means
   `run(mut c, await c.batch(sql))` keeps needing a local. **Proposed: reads only** (§3), as Rust does; it can be
   widened later.
3. **A flag to make warnings errors**, `--deny-warnings` on `check`, `build` and `test`, for CI? **Proposed: yes**,
   with the default as in §2.4: warnings never change the exit status unless asked.
4. **The B5 order fix** (§1) changes what `ps[i].set(bump(mut i))` does. It is a bug against language-spec.md, but
   a program could depend on it. **Proposed:** fix it in this release, and say so in the release notes. No program
   in the three repositories passes a `mut` index variable to an argument of a call on the indexed receiver
   (to be confirmed with a search before the release).
5. **Release:** everything here as 0.17.0. **Proposed: yes.**

## 7. Implementation (2026-11-05)

### 7.1 Where

- **Temporaries (§2.1):**
  - `borrowReceiver` (`src/check/places.cee`) borrows a receiver that is not a place as a statement temporary:
    `BorrowTemp` for a value, `BorrowPath` for a field or owned contents of one (NIR lowering spills the root);
  - it is used for the built-in receivers (`src/check/vectors.cee`, `bytes_text.cee`, `containers.cee`, `io.cee`,
    `library.cee`, `regions.cee`) and for `borrow mut this` user methods (`members.cee`); `borrow this` user methods
    already took a temporary;
  - slicing a temporary is the new `HirExpr.SliceTemp` (`src/check/arrays.cee`, lowered by `lowerSliceAt` in
    `src/nir/nir_builder.cee`); `.length` of an array value is `SliceLength(SliceTemp(…))`, so the value is still
    evaluated.
- **Two-phase arguments (§2.2):**
  - `lowerTwoPhaseOperands` (`src/nir/nir_builder.cee`) lowers the arguments of every call, method call, closure
    call and function-value call;
  - a `mut p` argument, or a reborrowed `borrow mut` local, has its place lowered where it stands (indexes copied and
    bounds-checked there), and its `Ref` made after the last argument;
  - a reservation is a shared `Ref` of the place, held in a temporary that is used just before the activations, so
    the borrow checker enforces the reads-only rule with its existing errors;
  - reads of a reserved variable, or reads through a reference, in later arguments are copied where they stand.
- **Value blocks (§2.3):**
  - `StmtNode.Value` (`src/syntax/ast.cee`) is made by `parseBlockOf(true)` for branch blocks
    (`src/syntax/parser.cee`), which decides with a balanced token scan (`endsValueBlock`) whether an ending `if` or
    `match` is the value;
  - `checkValueBlock` (`src/check/patterns.cee`) checks the block in its scope, and the value becomes the
    `HirStmt.Yield` that an expression branch already lowered to;
  - the statement walkers (closures' captures, `range_index`, the formatter, the AST printer) were given the new
    statement.
- **N0400 (§2.4):**
  - `warnStringBraces` (`src/check/string_braces.cee`) is a syntactic pass over the root package's files, run once
    after the bodies;
  - `Diagnostics.warning` and `denyWarnings` (`src/diagnostics/diagnostic.cee`);
  - `--deny-warnings` and the warnings printed by `build`, `run` and `test` (`src/main.cee`);
  - `\{` and `\}` (`src/syntax/lexer.cee`, the VS Code grammar);
  - the "Warnings" section of `diagnostics.md`;
  - `run_corpus.py` accepts `// warning[N0400]`, and `guide_check.py` a `warns` marker.
- **Spec and guide:**
  - type-system.md §13.2, §15.3, §16.2; grammar.md (`Branch`); lifetimes.md §5, §6; ownership.md B5; tooling.md §5.1;
    diagnostics.md;
  - guide/for-agents.md, guide/basics.md, templates/AGENTS.md.
- **Tests:**
  - run-pass `temporaries-as-receivers`, `two-phase-mut-arguments`, `value-blocks`;
  - check-pass `string-braces-warning`;
  - check-fail `temporary-view-kept`, `two-phase-errors`, `value-block-local-view`;
  - parse-fail `value-block-statement`;
  - `check-fail/arrays.cee` no longer expects N0216 and N0247 for `make().length` and `make()[0..1]`.

  The run-pass programs pass compiled, in the interpreter and with `--check-leaks`. `ps[i].set(bump(mut i))` prints
  `9` then `0`, where 0.16.1 printed `0` then `9`.

### 7.2 Changes from the accepted text

1. **Receivers keep B5's rule.** 0.16.1 accepts `v.push(v.pop()!)` and `p.set(bumpP(mut p))`, a mutable use of the
   receiver in a later argument. So the reads-only reservation (§2.2) applies to `mut p` arguments and reborrows,
   not to receivers. Receivers get only the left-to-right fix. Nothing that compiled before is refused.
2. **A reservation is made only when a later argument does more than read** (a call, a borrow, a move, a closure).
   Reads and arithmetic cannot change the place, and a read through a reference to it conflicts at the activation
   anyway. This keeps the generated C of `step(mut c, i)` unchanged.
3. **The reservation errors are the borrow checker's own:**
   - N0241 for a mutable borrow and for a shared borrow live at the call, with the note "borrowed here" at the `mut`
     argument, not "reserved here";
   - a move in a later argument (`takes(mut s, s)`) is N0250, a use after the move, rather than N0251.
4. **Slicing a temporary** covers array values and view or slice values (`s.view()[1..3]`, which was N0247 too). A
   vector or `String` value is sliced through `.view()`. `for (x in make())` is unchanged: its view must outlive the
   statement.
5. **The §2.1 example `let head = digest(data)[0..4];`** is N0243 when `head` is used afterwards. That is the rule
   working: a slice of an array temporary ends with its statement. A slice of a view (`s.view()[1..3]`) can be kept.
6. **N0400's text.** Notes print as `= note: … (line:col)`, so the advice is in the message:
   `this string prints '{x}' as written (there is no string interpolation): write the value in a call of its own, or
   '\{' to mean the brace`. There is no separate `help` line.
7. **The search in §2.4 missed one.** `src/packages/workspace.cee` has `"^{commit}"` in `hasCommit`, whose parameter
   is named `commit`, so it warns. The compiler's source must still build with stage 0 (0.3.2), which rejects `\{`,
   so it is now written as two appends, `"^{"` and `"commit}"`.
8. **Frames** (`Scratch.enter().alloc(1)`) stay N0247: a frame must be a local (type-system.md §19.5), a rule of its
   own.
9. **`guide_check.py`** gains a `warns N0400` marker. It is `fail` for warnings, but the check must succeed.
