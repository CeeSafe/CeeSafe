# ADR 0212 — A nested pattern matches through a reference payload

- Status: **Accepted** by the owner, 2026-11-23 ("commit. And accept ADR 0212"), with §6's two questions taken as
  recommended; proposed the same day and implemented under the owner's instruction ("continue", the optimization and
  DX roadmap's Phase 3, item 6); not released. It is ADR 0195's question 4, which that ADR left to "an ADR of its
  own".
- Scope: type-system.md §15.2; `src/check/patterns.cee`, `src/check/usefulness.cee`, `src/nir/nir_builder.cee`.

## 1. Context

A lookup returns an optional reference, and the natural way to use it is refused:

```ceesafe
fn count(all: borrow Vector<Node>) -> usize => match (first(all)) {     // first: borrow Node?
    Some(Node.Items(v)) => v.length,    // N0205: this payload is a reference ('borrow Node'): bind it ('Some(n)')
    _ => 0,                             //        and match on 'n' separately
};
```

§15.2 let a payload that is a reference take only a name or `_`. ADR 0195 §2.2 made the refusal say what to write and
stop it from cascading, and recorded that matching through the reference "is sound: it moves nothing, and is what the
two-step form does", leaving it undecided because it "reaches the usefulness algorithm, NIR's match lowering, the
interpreter, and ADR 0123's struct fields". The two-step form costs a second `match` and, where a later arm should
catch what the inner one does not, a repeated arm.

## 2. Decision

A nested pattern on a payload, or a struct pattern's field, whose type is a reference **matches what the reference
refers to**, when it is a variant, struct, literal or range pattern, or alternatives of them:

- Every name bound under it is a reference into the referent, as on a reference scrutinee (§15.2). It is a
  `borrow mut` only where the payload is one and the match is not itself through a shared reference.
- Nothing moves. A consuming match that goes through a `borrow mut` payload so reborrows it.
- Coverage and unreachable arms are decided over the referent's constructors: N0256 names `Some(Node.Items(_))`.
- It nests: `Some(Outer.One(Node.Leaf(v)))` on a `borrow Outer?` whose `One` holds a `borrow Node`.
- `match`, `is`, `let … else` and guards alike.

Unchanged: a slice or `StringView` payload takes string and slice patterns (ADR 0085 §2.7). `name @ P` on a reference
payload stays N0205 with ADR 0195's message: `name` would be the reference and `P` its referent, two things one
pattern position does not say. A box payload is a value, as before.

## 3. Options not taken

- **A `borrow` pattern** (`Some(borrow Node.Items(v))`), the reference written in the pattern. One more thing to
  write for what the type already says, and nothing else it could mean: the refused program has one reading.
- **Only optional references**, the case every package meets. An enum's own reference payload and a struct's field
  are the same rule, and ADR 0195's message already treated them as one.

## 4. Consequences

- Programs that were N0205 compile; none that compiled changes. A reference payload with only names and `_` is
  matched as before, and the coverage of such a column is unchanged (no arm names a constructor of the referent).
- The lowering reads through the payload's pointer before the inner test, which the two-step form did too.

## 5. Implementation and tests (2026-11-23)

- `matchesThroughReference` and `referenceThrough` (src/check/patterns.cee): the nested pattern is checked against
  the referent, with the reference as §15.2's reference scrutinee. No new HIR pattern: the lowering sees a variant,
  struct, literal, range or or-pattern on a place whose type is a reference and tests or binds the referent
  (`matchesReferent`, src/nir/nir_builder.cee), which the checker lets nothing else reach. The interpreter and the C
  backend run the NIR.
- `testedType` (src/check/usefulness.cee): a reference column's constructors are its referent's.
- run-pass/patterns-through-references.cee (30 lines of output, the C build and the interpreter);
  check-fail/patterns-through-references-errors.cee and -moves.cee. Three files edited:
  check-fail/adr0195-reference-payload-names.cee now refuses `name @ P` (its point, the names declared, stands);
  nested-patterns.cee and struct-patterns.cee each lose one N0205 that is now allowed. The results are in
  ROADMAP §10.

## 6. Questions for the owner (answered 2026-11-23: accepted as recommended)

1. Should `name @ P` on a reference payload bind `name` as the reference and match `P` through it? Not now; it is
   rare, and the refusal says what to write.
2. Should a constant's name on a reference payload (`Some(LIMIT)` on a `borrow u32?`) match through it as a literal
   does? Yes, as a follow-up; it is not done here.
