# ADR 0014 — C arrays in imported structs, C member names, and exported constants

> **Inherited.** ADR 0014 was accepted before CeeSafe's fork (Decision 0006), when the language had its
> earlier name. In this record, "the reference" is that language and project and "the reference compiler" its compiler:
> read them as CeeSafe and `ceesafec`. Links to documents CeeSafe did not keep are given as text.

- Status: **Accepted** (project owner, 2026-10-26: "start C9 with the GPT parser", then "You can go ahead with any
  integration you need"). The design below is the agent's under that instruction; the owner may revise it.
- Scope: the additions to C interoperability that C9 needs (`docs/cosmoos/c9-report.md`): CosmoOS's GPT
  parser (`kernel/block/gpt.c`) cannot be written in the reference with the API of `kernel/include/kernel/gpt.h`
  without them. ADR 0008 named C arrays as a "later addition"; this is it.

## Context

`gpt.h` is an ordinary C header, and its shapes are common in kernel and library headers:

- `struct gpt_table` holds `uint8_t disk_guid[16]` and `struct gpt_part parts[32]`; `struct gpt_part` and
  `struct gpt_build_part` hold `uint8_t type[16], uuid[16]`. An imported struct's fields could only be
  integers, `bool`, raw pointers, function pointers and embedded imported structs (type-system §31.1, §33.1).
- One member is named `type`, a CeeSafe keyword. A field had to be declared with its C name.
- `gpt_build` takes `const struct gpt_build_part *parts, unsigned nparts`: an array of imported structs behind
  a pointer. `Ptr.add` needs the pointee's size, which the reference does not know for an imported type (§31.2).
- `gpt.h` declares data: `extern const uint8_t gpt_type_esp[16]` and `gpt_type_cosmo_root[16]`. The reference could
  export functions only, and had no constant arrays at all (`const T: [u8; 2]` was N0216).

## Decision

1. **Array fields of imported structs.** A field of an imported struct may have type `[E; N]`, where `E` is an
   integer, `bool`, or an imported type. Its C member must be `CE name[N]`: the generated C asserts
   `_Generic(&member, CE (*)[N]: 1, default: 0)`, which is exact, and that the member has the size of the reference's
   array. Inside the reference the field is an ordinary array place: index it (bounds-checked against `N`), slice it,
   borrow it, and, when `E` is Copy, read and assign it whole.
2. **Arrays of imported types are C-laid-out.** `[T; N]` with `T` imported (or C-laid-out) is itself C-laid-out
   (§33.1): allowed where an imported type by value is (a field, an embedded member, a box's contents), never a
   value. Indexing it gives a place of `T`, used through its fields or borrowed.
3. **Lowering by the same representation.** the reference's `[E; N]` is `struct { E e[N]; }` in C (`c-backend.md` §2),
   with the size and alignment of `E[N]`, and an access through the struct type is an access through an
   aggregate that contains the member's type (C11 6.5p7). An array field place is `(*(ceeA… *)&p->member)`: no
   copy, and every operation on arrays works unchanged. The generated C asserts the sizes are equal.
4. **`@cname("name")` on a field of an imported struct** gives its C member name when that name is not a usable
   CeeSafe identifier (a keyword: `type`, `match`, `in`). CeeSafe code uses the CeeSafe name.
5. **Pointer arithmetic on pointers to imported types.** `add`, `sub` and `offset` on `*T` / `*mut T` with `T`
   imported are allowed (still unsafe, §27.3): C scales by its own `sizeof`. `read`, `write` and `fromRaw` stay
   N0294, since they would copy a value or make a slice of one.
6. **Constant arrays.** `const NAME: [E; N] = [ … ];` (or `[v; N]`), with `E` an integer or `bool` and every
   element a constant expression (§10). A use is the array's value, as for any constant.
7. **Exported constants.** `@export("symbol")` (or `@export`) on a constant of integer, `bool` or such array type
   defines C data: `const E symbol[N] = { … };` (or `const T symbol = v;`), declared in `ceesafe_exports.h` as
   `extern const E symbol[N];`. It goes in read-only data, which C4's object checks allow.

## Consequences

- type-system §10 (constant arrays), §28.1 and §28.4 (exported constants, `@cname`), §31.1 to §31.3 (array
  fields, `@cname`, pointer arithmetic) and §33.1 (arrays of imported types) change; `c-backend.md` §2.3
  describes the lowering.
- The interpreter has no layout for imported types: accessing an array field, or `add` on a pointer to an
  imported type, is an interpreter fault, as other imported-field accesses are (§31.4). Constant arrays and
  exported constants work in the interpreter.
- A header's `char name[N]` (plain `char`) cannot be declared: `CChar` is pointee-only (§28.3). Bit-fields,
  unions, anonymous members and flexible array members remain later additions.
- An exported function whose C prototype in the header uses an `enum` type (`enum gpt_result gpt_parse(…)`)
  is defined by the reference with the enum's underlying integer type. GCC and Clang make an enum without negative
  values `unsigned int`, so the two declarations are compatible and the header can be included; a compiler
  that chose another type would reject the build (it could not miscompile). This is recorded in the C9 report
  as a property of those compilers, not a reference rule.
