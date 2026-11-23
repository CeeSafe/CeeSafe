# ADR 0215 — Type aliases: why not, and what would change that

- Status: **Proposed** 2026-11-23, at the owner's instruction ("continue with 3.9": the optimization and DX
  roadmap's Phase 3, item 9, "ADRs for the ergonomic gaps"). It proposes no change. §5's questions are the owner's
  to decide.
- Scope: diagnostics.md N0101 and the agents template's "Common mistakes", the two places that say there are none
  (the specification has no rule for a form it does not have).

## 1. Context

`type Id = i32;` is N0101, which since the roadmap's item 1.5 says "CeeSafe has no type aliases; write the type
where it is used, or declare a struct that holds it". The agents template lists it among the common mistakes. No ADR
said why. ADR 0079 §3 declined an alias once, for a C enum's typedef ("an alias would still be spelled
`uint32_t`").

What an alias would be used for, counted on 2026-11-23 over the applications (CeeSafeApps without its worktrees:
1,651 files, 674,164 lines):

| What repeats | The most repeated | Times |
|---|---|---|
| a `Result` with a fixed error | `Result<Vector<u8>, AllocError>` | 127 |
| | `Result<TablesAnswer, TablesError>` | 45 |
| | `Result<BlobWriter, StoreError>` | 42 |
| a shared handle | `Arc<AsyncMutex<ObjectStore>>` | 45 |
| a function type | `borrow mut fn(u64, u64, borrow RecordHeader, Slice<u8>)` | 3 |

The longest type that repeats often is 30 characters. No function type repeats more than three times.

## 2. Decision

**No type aliases.** The reasons, in the order they weigh:

1. **A name in a signature is its own definition.** A reader, and an agent, sees `Result<BlobWriter, StoreError>`
   and knows `?` applies, what the error is and what is owned. `StoreResult<BlobWriter>` says none of it without a
   lookup. The language's other choices go the same way: one canonical format, no implicit conversions, `borrow`
   written at the call.
2. **Every tool that prints a type would have to choose** between the alias and what it stands for: diagnostics,
   hover, `ceesafec doc`, the API pages, `ceesafe_api`. Either choice loses something (the alias the author wrote, or
   the type the rule is about), and two types that print differently and are the same type make N0205 harder to
   read, not easier.
3. **The measured need is small.** §1's repeats are short, and the one long, repeated shape (a handle such as
   `Arc<AsyncMutex<ObjectStore>>`) is better as a struct with one field: it then has a place for its methods, and
   the type checker tells two handles apart.
4. **A distinct type is already one line.** `struct Meters { value: f64; }` is the newtype an alias is often
   wanted for, and it is checked.

## 3. Options not taken

- **Transparent aliases, top level, not generic** (`type Id = u32;`). Serves constants' types and little of §1:
  the repeats are `Result<T, E>` with `E` fixed, which need a generic alias.
- **Generic aliases** (`type StoreResult<T> = Result<T, StoreError>;`). The form that would be used, and the one
  reason 1 is about.
- **Aliases for function types only.** The one place a type is long enough to hurt, and it repeats three times in
  674,000 lines.

## 4. What would change this

- A port that carries hundreds of uses of one long type that is not a `Result` and cannot be a struct.
- Function types becoming common in signatures (callback-heavy libraries), which the count above would show.

## 5. Questions for the owner

1. No type aliases, for §2's reasons? Recommended: yes.
2. Should the agents template and N0101's message say "declare a struct with one field" for the handle case, as §2's
   third reason does? Recommended: yes, a sentence in the guide's types page; N0101 already says it.
