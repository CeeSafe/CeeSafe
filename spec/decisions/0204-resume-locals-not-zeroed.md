# ADR 0204 — A resume function's locals are zeroed only when they must be

- Status: **Accepted** by the owner, 2026-11-21 ("Take both in one run", to the CeeSafe side of per-request isolation's
  cost found by an independent benchmark of httpserver); implemented, released in 0.91.0.
- Scope: the C backend's declarations of an async function's C locals (`src/backend/c_async.cee`) and its rule for
  which aggregate locals start zeroed (`partlyWritten`, `src/backend/c_body.cee`); c-backend.md §3b and §5. No
  language, library or runtime change.

## 1. Context

A sync function's aggregate local has had no initializer since M9d when it is assigned whole before every read; only
one written in part, borrowed, or (for gcc's `-Wmaybe-uninitialized`) read as a variant's payload is zeroed. An async
function's resume function did not follow the rule: every C local (one not live across a suspension point) was
declared `= {0}`, at every entry of the resume function, which runs again at each resume.

httpserver's per-request isolation (CeeSafe.Web's Web ADR 0017) serves 30 to 40 % fewer requests a second than
isolation per connection. On a Xeon D-2123IT, one worker on one core, a profile of the isolated relay put about
1.4 µs of each request's 6 µs extra in `exchangeIsolated`'s resume function, nearly all of it `rep stos` and
`rep movsq`: its 56 C locals, 3 042 bytes, each zeroed by a store of its own on every entry, the fixed cost of each
`rep stos` more than its bytes.

The payload-read reason is obsolete as well: `rt.h` turns gcc's `-Wmaybe-uninitialized` off (CeeUsb's F9,
2026-11-19), and such a local is assigned whole before it is read.

## 2. Decision

- A resume function's C local is declared as a sync function's: zeroed when written in part or borrowed (or an
  `Unpack` target), else with no initializer. Such a local is not live across a suspension point, so every read of it
  follows a whole assignment in the same resume.
- A variant payload read no longer zeroes a local, in sync and async functions alike.
- References and scalars keep their zero initializer, as before: the C compilers remove those stores.

## 3. Consequences

- In httpserver's benchmark server, zeroed locals in the generated C fell from 5 279 to 717; in `exchangeIsolated`'s
  resume function from 56 to 4.
- No program's behaviour changes: definite assignment (the checker's, and the interpreter's on every run) already
  forbade reading such a local before it is assigned.

## 4. Implementation and measurements

`src/backend/c_async.cee` (the resume function's declarations), `src/backend/c_body.cee` (`partlyWritten` without the
payload reads; `addPayloadRead` and `addPayloadReads` removed).

httpserver's `bench` on the Xeon, one worker on one core, `/small` at 64 connections, four alternating runs each, the
load on cores no other work used:

| | isolated req/s | direct req/s |
|---|---|---|
| 0.89.0 | 46 691 (one run of 42 216, the others 48 001 to 48 313) | 73 278 |
| ADR 0204 | 48 534 | 72 718 |
| 0.89.0, CeeSafe.Web's Web ADR 0022 | 49 286 | |
| ADR 0204 and Web ADR 0022 | 51 280 | |

The contract suite on Linux x86-64: see §5.

## 5. Checks

The contract suite on Linux x86-64 (Ubuntu 24.04, gcc 13 and clang 18), on this change built on 0.89.0:

- `run -Werror` (429 run-pass, 55 run-fail, 22 ffi), `run asan` (the same under AddressSanitizer and UBSan with leak
  checks), `run fortify` (gcc -O2 -Wall -Werror -D_FORTIFY_SOURCE=2), `run split`, the three ThreadSanitizer steps,
  both reactors' async programs, the fault, cancellation, support and hash-map sweeps, the fixed point, and every
  runtime check (slices, bound, isolated, debugger and the rest): ok.
- `corpus`: every family passes but `emit-c`, whose 11 snapshots differed by exactly the initializers dropped, 24
  lines, each `= {0};` become `;`; updated by that edit alone, after which the family passes (38 of 38).
- `run interp`: `run-pass/ordered-map.cee` fails in the interpreter, as with 0.89.0 on the same machine (428 of 429
  both); the interpreter does not use the C backend.
- `packages`: `records` fails ("a cache was made in a home that is not writable"), as with 0.89.0 there: the suite
  ran as root, whose home is writable.
- `release chain`: skipped, no stage 0 on that machine.
