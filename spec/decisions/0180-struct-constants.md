# ADR 0180 — Constants of plain struct type

- Status: **Accepted** by the owner, 2026-11-20 (asked whether to accept the designs proposed under "fix all findings": "Accept all four"); proposed 2026-11-19, done under the owner's instruction to fix every finding of CeeUsb ("Now we focus on CeeSafe and fix all findings"). Implemented 2026-11-19; released
  in 0.79.0.
- Scope: type-system.md §10. Finding F8 of CeeUsb's `CEESAFE_FINDINGS.md`.

## 1. Context

Windows' device APIs name things by `GUID` and `DEVPROPKEY`: plain structs of integers and a byte array. A constant
of struct type was N0216 ("not supported yet"), so CeeUsb built each one in a function, at run time, on every call.

## 2. Decision

- A constant's type may be a **plain struct**: no drop, no region, every field an integer, `bool`, float, `CChar`, an
  array of these, or a plain struct. A constant array's element may be one too (a table of rows).
- Its value is the struct's literal with constant fields (calls are N0217), or another such constant. It is folded
  field by field when the constant is evaluated; a use is the folded literal, as a constant array's use is the
  array's value.
- The check is made when the constant is evaluated, not declared, since a struct's fields may not be resolved yet
  when its constants are declared. A struct that owns memory or holds a loan is N0216 at the constant's type (a region
  parameter is N0246, as any reference type is).

## 3. Options not taken

- **Any Copy struct**: a struct with a raw pointer field is Copy, but a constant pointer is an address chosen at
  run time; plain data only.
- **A `static` instead**: a static is one object with an address, which is a table's job (§39.2); a GUID is a value
  copied into each call.

## 4. Implementation and tests (2026-11-19)

- `constStructOk` (src/check/declare.cee), the struct case of `fold` (src/check/consts.cee), the use in
  `namedConstantValue` (src/check/expr.cee).
- `run-pass/adr0180-struct-constants.cee`: a GUID, a struct holding one, a table of three points; the expected values
  computed apart in Python. `check-fail/adr0180-struct-constants.cee`: a struct with a `Vector` (N0216), one with a
  region (N0246), a call (N0217). Two older expectations reviewed: `check-fail/structs.cee`'s constant made by a call
  is now N0217 (the type is allowed), and `check-fail/c-arrays.cee`'s table of `Pair` is now valid.
