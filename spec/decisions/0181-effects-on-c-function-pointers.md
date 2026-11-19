# ADR 0181 — Effect lists on C function pointer types, and effect aliases of C functions

- Status: **Accepted** by the owner, 2026-11-19 (asked about CeeUsb's finding F7: "Effects on fn-pointer types +
  aliases"). Implemented the same day released in 0.79.0.
- Scope: type-system.md §28.2, §29.5, §36.3; grammar (the type `@effects(…) extern "C" fn(…)`). Finding F7.

## 1. Context

Effects (§32) bound what async functions and atomic contexts may call. A call through a C function pointer counts
as `sleep` and `alloc`, and `extern "C" fn` types took no effect list. IOKit's user-client API is COM: every call is
through a vtable of C function pointers, so `ReadPipeAsync`, which returns at once, made each async caller N0295.
One level down, `ioctl` is one C function for blocking requests and for ones that return at once, and its one
declaration can carry one effect list; a second declaration of the name was N0201. CeeUsb wrapped each such call in
a C trampoline declared `@effects()`.

## 2. Decision

1. **The type.** `@effects(e, …) extern "C" fn(…) -> R`, the list written before `extern` as before `fn` (§36.3). The
   list is part of the type; without one it is both effects, as before. Calls through it have the list's effects.
2. **Values.** A named C function (extern, `@export`, `@callback`) where such a type is expected is checked after
   effect inference: its effects must be in the list (N0295, reported where it is named). Any other value converts
   only to a type whose list includes its own (N0299); a pointer with a list converts to one without.
3. **Aliases.** A second extern declaration of a C function, under its own CeeSafe name with `@cname`, is allowed
   when it differs only in effects: the same parameter and result types, variadic-ness and `unsafe`. Each call has
   its declaration's effects. Anything else stays N0201.
4. **C.** Nothing changes: the types are one C type; an alias is the same symbol.

The list on a pointer type is CeeSafe's promise about C it cannot see, as `@effects` on an extern declaration already
is: the program's author makes it, where the type or declaration is written.

## 3. Options not taken

- **Effects per call site** (`unsafe(effects: none) { f(x) }`): the promise would be repeated at every call, and a
  COM interface's vtable is declared once.
- **Overloading by effects** (one name, two declarations): CeeSafe has no overloading; a second name is explicit.

## 4. Implementation and tests (2026-11-19)

- Parser and AST (`TypeNode.FunctionPointer`'s fourth field, the attributes), `TypeTable.functionPointerWith`, the
  conversion beside `fn` types' (src/check/expr.cee), `functionAddress`, `typeEffects` and the call's effects
  (src/nir/effect_check.cee), `isEffectsAlias` (src/check/ffi.cee), the formatter, and the interpreter's C calls by
  their C symbol.
- `run-pass/adr0181-c-pointer-effects.cee` (native and interpreted: a `@callback` called through such a pointer from
  an `@effects()` function and an async fn; a list converted to none; `abs` and an `@effects()` alias of it).
  `check-fail/adr0181-c-pointer-effects.cee` (N0295 twice), `check-fail/adr0181-c-pointer-types.cee` (N0299, and N0201
  for an alias whose parameter type differs). `parse-fail/effect-list-placement.cee` tested the old refusal; it now
  tests an effect list before an array type (reviewed).
