# ADR 0216 — Tuples and anonymous result structs: why not

- Status: **Accepted** by the owner, 2026-11-24 ("I accept ADRs 0215–0217"), §5's questions as recommended: no tuples
  and no anonymous result structs, and no `Pair<A, B>` in lib/core. No change to the language. Proposed 2026-11-23 at
  the owner's instruction ("continue with 3.9": the optimization and DX roadmap's Phase 3, item 9).
- Scope: type-system.md §12 and §15; ADR 0018 §2 ("the reference has no tuples"), ADR 0032, ADR 0085 §4, ADR 0117,
  ADR 0123 §1 ("CeeSafe has no tuples and is not getting them"); diagnostics.md N0100.

## 1. Context

`(1, 2)` and `-> (i32, i32)` are N0100, which since the roadmap's item 1.5 says "CeeSafe has no tuples; declare a
struct for the values". Five ADRs mention the absence and work around it (a channel's two ends, a thread's result,
`let … else`'s bindings, matching two values together); ADR 0123 says in passing that tuples are not coming. None
says why.

What programs do instead, counted on 2026-11-23 over the applications (1,651 files, 674,164 lines):

| Instead of a tuple | Count |
|---|---|
| a struct with exactly two fields and no functions | 383 of 2,410 struct declarations (16%) |
| a function with a `borrow mut` parameter of a scalar type (an out-parameter) | 94 |

The two-field structs are named `Span`, `LineSpan`, `SeqRange`, `Hop`, `DomainEntry`, `Mailbox`, `Flag`,
`Delivered`: their names say what the pair is. lib/core does the same for its own pairs (`TextHalves`,
`ByteHalves` for `splitOnce`).

## 2. Decision

**No tuples, and no anonymous result structs.**

1. **A pair's fields have names here.** `halves.before` and `halves.after` are read without a comment; `.0` and
   `.1` are not. The 383 structs above are evidence that the name was wanted where a pair was returned.
2. **A struct pattern already takes a pair apart** (ADR 0123): `let TextHalves { before, after } = line.splitOnce(":")
   else { … };`. What a tuple pattern would add is the absence of the type's name.
3. **Matching two values together has its forms**: a nested `match`, `is` chains, and a struct pattern over a
   struct built for the purpose (ADR 0085 §4, ADR 0123).
4. **One more aggregate kind is one more of everything**: layout, Copy and drop rules, equality, generics over
   arity, patterns, the C mapping, the interpreter, `ceesafec doc`. Each exists for structs.
5. **An anonymous result struct** (`-> { before: StringView, after: StringView }`) keeps the names and drops only
   the type's. Two functions that return "the same" anonymous struct then return different types or the same one,
   and either answer surprises; a named struct has no such question.

The out-parameters (94) are the weaker substitute: they are used where one of two results is a count or a flag.
They are not a reason for tuples; a two-field struct serves them as it serves the other 383.

## 3. Options not taken

- **Tuples as structs with positional fields**, Copy and laid out as a struct is. §2's first and fourth reasons.
- **Anonymous result structs.** §2's fifth reason.
- **A library `Pair<A, B>`.** It can be written today (`struct Pair<A, B> { first: A; second: B; }`) by any package
  that wants it; in lib/core it would be the tuple without the syntax, and with `first` and `second` for names.

## 4. What would change this

A finding from a port where declaring the result structs is a measurable share of the work (ADR 0085 counted 4,058
tuple pattern arms in one Rust code base; none of the ports recorded so far lists the missing tuple as a finding).

## 5. Questions for the owner

1. No tuples and no anonymous result structs, for §2's reasons? Recommended: yes.
2. Should lib/core have `Pair<A, B>` (§3)? Recommended: no.
