# ADR 0097 — A dependency's interface named in an `impl`

- Status: **Accepted** by the owner, 2026-11-11 ("I accept both ADRs"), every question as proposed: the qualified form is accepted, in a patch
  release. **Implemented** (§6); released with ceesafec 0.35.0. Raised by the port of two tsrs crates
  (`~/dev/CeeSafeApps/bun/analysis/trials/tsrs/REPORT.md` §4, gap 2).
- Scope: `spec/type-system.md` §2, `spec/diagnostics.md` (N0315), the reference implementation's `PackageNames`,
  ceesafec's `src/check/packages.cee`. No grammar change.
- Release: the next patch release, in one stage. **Small.**

## 1. Context

- **What the port met.** A package implemented an interface of a dependency, naming it as every other use of a
  dependency's item is named:

  ```ceesafe
  impl tsrs_core.Order<Segment> for ByVirtualStart {
      public fn less(a: Segment, b: Segment) -> bool => a.virtualStart < b.virtualStart;
  }
  ```

  ```text
  error[N0315]: 'tsrs_core' is the name of a dependency and cannot also name an item
  ```

- **Why.** N0315 refuses an item that has a dependency's name. An impl has no name of its own, so the check takes
  its interface's first identifier, which here is the dependency. The reference implementation does the same, and
  ceesafec follows it (`rewritePackageNames`).
- **The grammar already allows it.** `ImplDecl = 'impl' Type 'for' Type …` and `Type = QualifiedName …`
  (`grammar.md`), and §2 says a dependency's public items are reached by qualifying them, in a type among others.
- **What works today.** `import tsrs_core.Order;` and then `impl Order<Segment> for …`. The message does not say so:
  it reads as if the package had declared an item named `tsrs_core`.
- **The orphan rule is not involved.** The impl is in the package that declares the target struct (§21.1).

## 2. Proposal

- **An impl is not a declaration of a name.** N0315 no longer looks at impls: neither the interface nor the target
  of an impl declares anything in the package's scope.
- **`impl dep.I<…> for T` resolves the interface as any qualified type does**, and means what the imported form
  means. The target may be qualified too where the orphan rule allows the impl (`impl Local for dep.T`, with
  `Local` this package's interface).
- **Both implementations change together:** the reference's `PackageNames` and ceesafec's `rewritePackageNames`,
  with §2's list of where a qualified name is accepted gaining "the interface and the target of an `impl`".
- **N0315 keeps its other cases:** a struct, enum, function, constant, interface or type parameter named as a
  dependency.

## 3. Alternatives considered

- **Keep the rule and fix the message:** "name the interface through an import: `import tsrs_core.Order;`". Smaller,
  but the one place in the language where a dependency's item cannot be named by qualifying it stays, against the
  grammar and §2.
- **Nothing.** The import works. The cost is the message, which sends the reader looking for an item that does not
  exist; a model writing CeeSafe will write the qualified form first, as this port did.

## 4. Testing

- **Corpus, run-pass (packages):** `impl dep.I for T`, `impl dep.G<u8> for T` and `impl Local for dep.T`, each
  called through a bound and through `dyn`, beside the same impls written with an import; native and interpreted.
- **Corpus, check-fail:** N0315 still for `struct dep { … }` and a type parameter named `dep`; `impl dep.Missing for
  T` reports the unknown item (not N0315); `impl dep.I for dep.T` is the orphan rule's error.
- **The port:** the trial's `tsrs_spanmap` drops its `import tsrs_core.Order;` and qualifies the five impls.

## 5. Questions for the owner

1. **Accept the qualified form, or keep the rule and fix the message?** **Proposed: accept it.** The grammar and §2
   already describe it; only the N0315 check stands in the way.
2. **In a patch release?** It accepts programs that were errors and changes no program that compiled.
   **Proposed: yes.**

## 6. Implementation notes (2026-11-11)

On branch `impl/0097`, on 0.33.0 with ADR 0093.

- **The change** is in `rewritePackageNames` (`src/check/packages.cee`): impls, interface and inherent, are left out
  of the names N0315 checks, as extern blocks are. Nothing else changed: the interface and the target of an impl are
  type nodes, which the same pass already joins into `dep.X`, so they resolved as soon as the check let them through.
- **The reference implementation is not changed.** §2 said both change together; the reference is the frozen fork
  point (`tests/reference/README.md`), and this is a CeeSafe-only change, as the others since Decision 0007.
- **What the other cases report:** `impl dep.I for dep.T` is N0267 (the orphan rule), and an interface the
  dependency lacks is N0311.
- **Tests:** `tests/packages/qualified-impls`, check 29 of `tools/package_tests.py`, in place of §4's corpus files:
  the corpus has no dependencies.
