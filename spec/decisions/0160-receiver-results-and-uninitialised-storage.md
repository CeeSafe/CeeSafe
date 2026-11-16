# ADR 0160 — A method's result borrows from its receiver, and the end of an uninitialised local's storage reads no loan

- Status: **Accepted** by the owner, 2026-11-16 ("Go ahead to ~/dev/CeeSafe repo and fix all findings first.", the
  findings of `~/dev/CeeSafeApps/httpclient/CEESAFE_FINDINGS.md`; then "Go ahead and cut a new release of CeeSafe."). The
  rule of §2 is this ADR's choice among the ways to fix finding F1. Implemented on branch `fix/httpclient-findings` from
  `main` at `8fedc5fd` (release 0.60.0); released in 0.61.0.
- Scope: type-system.md §13.5, §25.6 and §25.7 rule 6; lifetimes.md §3; the checker's `resolveReturnSources` and the
  borrow checker's liveness. One new message under N0245; no new diagnostic code, and no NIR, C backend, interpreter
  or library change.

## 1. Context

Both findings come from writing an HTTP client (`cosmohttp`) whose response lends views of a connection's buffers.

- **Finding F1.** A lookup is a method that takes its key and lends what it finds: `map.get(key)`,
  `response.header(name)`, `client.get(url)`. With `borrow this` and one more scoped parameter (a `StringView`, a
  slice, a `borrow T`), a result that is a view or a struct with a region was N0245, "this function has several
  reference parameters: name the ones the result is borrowed from". A `borrow T` result has an answer,
  `-> borrow(this) T`. A scoped result had none: `borrow(this) Slice<u8>?` is N0246, `borrow(r) this` and
  `Slice<this, u8>` do not parse, and §25.6 names another parameter's region but not the receiver's. The only form
  that checked was a static function, `Response.header(response, name)`, so a library's most used call was not a
  method, or returned a copy (web_client's `ClientResponse.header` returns a `Vector<u8>`).
- **Finding F6.** In a loop, a local and a value that borrows it were N0240 and N0243 when the borrower's type has a
  `drop` block and it is made through `?`:

  ```ceesafe
  for (k in 0..3) {
      let owner = Owner { … };          // N0240: cannot assign to 'owner' while 'owner' is borrowed
      let held = lease(owner)?;         // Lease<c> has a drop block
  }                                     // N0243: 'owner' goes out of scope while still borrowed
  ```

  The same body outside a loop, or with a `Lease` without `drop`, was accepted. The cause: §25.7 rule 6 makes
  destroying a value whose drop may read its loans a use of them, and the checker took every `StorageDead` of such a
  local as that use. On the path where `?` returns, `held` was never assigned, and the temporaries of `lease(owner)?`
  have been moved out of: nothing is destroyed there. Taken as a use, that `StorageDead` kept the local live from the
  top of the loop's body, where it still held the last turn's loan of `owner`.

## 2. A method's result borrows from its receiver

**In a method whose receiver is of scoped type** (`borrow this`, `borrow mut this`, or a `this` whose type holds a
borrow) **and which has other scoped parameters, a result of scoped type with no source written borrows from the
receiver.** "No source written" is as before (§25.6): no `borrow(…)` list and no region on the result's type.

```ceesafe
struct Table {
    bytes: Vector<u8>;

    fn find(borrow this, name: StringView) -> Slice<u8>? => …;      // borrows from `this`
    fn lease(borrow this, name: StringView) -> Lease => …;          // a struct with a region: the same
    fn slot(borrow mut this, key: StringView) -> borrow mut u32 => …;
}
```

- The result is tied to the receiver exactly as the result of a method whose only scoped parameter is the receiver
  already is: the body may return only what is derived from the receiver (N0245 for another parameter's loan), and a
  caller's receiver stays borrowed while the result is used. It is not a deep result (ADR 0074 §2.4).
- A `borrow mut` result needs a `borrow mut` receiver. From a shared one it is N0245, with a message of its own: "a
  'borrow mut' result cannot be derived from the shared receiver: write 'borrow mut this', or name the result's
  sources, e.g. 'borrow(a) mut T'".
- Nothing else changes. A static function with several scoped parameters still names its sources (it has no
  parameter that stands out), and so does a method whose receiver holds no loan (`this` by value of a type without
  regions). A result that names a region, or lists sources, means what it says: a method that lends from another
  parameter writes `fn first<region a>(borrow this, other: Slice<a, u8>) -> Slice<a, u8>` or `-> borrow(other) T`.
- A result whose written type names a type parameter is not covered (`fn run(borrow this, o: borrow Owner, f:
  fn(StringView) -> U) -> U`): it is scoped only in an instance, and what a type parameter holds is followed through
  the declaration's flows (§25.7 rule 5, ADR 0131). Such an instance is N0245 at the declaration, as before (the
  corpus's `instance-flows-bound`, `-default-method` and `-dyn`).
- A result reference with no region on the reference whose pointee names regions (`-> borrow Inner<r>`) keeps
  §25.6's rule: with several scoped parameters it is not elided. Such a signature checks today as a deep result;
  tying it to the receiver would reject callers that are accepted now.

**Why elision, and not a way to name the receiver's region.** `borrow(r) this` (or `Slice<this, u8>`) would say the
same thing with more to write, in every lookup of every library, and would need new syntax in the receiver. The
rule here is the one a reader expects of a method, and every program it changes was an error before. A method that
needs both the receiver and another parameter as sources of a scoped result still cannot say so; none has been met,
and the receiver's region can be given a name when one is.

## 3. The end of an uninitialised local's storage reads no loan

§25.7 rule 6 becomes: destroying a value that is scoped and needs drop is a use of its loans, **where the value may
be initialised**. A `StorageDead` of a local that is certainly not initialised there (never assigned on any path to
it, moved out of, or its storage already ended) destroys nothing, and is no use.

- "Certainly not initialised" is the move analysis's (`ownership.md` §4): no leaf of the local's move path is
  maybe-initialised before the statement. It is what drop elaboration uses to decide that no drop is emitted there,
  so the checker and the generated code agree by construction.
- A local that is initialised on some path to its `StorageDead` is as before: the drop may run, and reads its loans.
- The checker runs the move analysis a second time only for a function that has a `StorageDead` of a local whose
  drop reads loans; every other function is checked as before.

## 4. Tests

Corpus: run-pass `adr0160-receiver-result` (a view, an optional of one, a reference, a `borrow mut`, a struct with a
region and a `Result` of one, each from a method with a second scoped parameter) and `adr0160-loop-drop-borrower`
(the loop of §1, with the drops' output); check-fail `adr0160-receiver-result` (a `borrow mut` result from a shared
receiver, a receiver that holds no loan, a static function), `adr0160-receiver-result-borrows` (a body that returns
another parameter's loan, a caller that changes the receiver while the result lives) and `adr0160-drop-reads-loans`
(a local that is, or may be, initialised where its storage ends is still a use).
