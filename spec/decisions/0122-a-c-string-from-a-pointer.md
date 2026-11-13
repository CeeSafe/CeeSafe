# ADR 0122 — A C string from a pointer: `CStr.fromPtr`

- Status: **Accepted** by the owner, 2026-11-13 ("Fix those new gaps too"), implemented on branch `fix/gaps2-d`; released in ceesafec
  0.42.0. Raised by the book's authors (gaps2 D13): reading a `const char *` that C returns took three steps,
  a declaration of `strlen`, `Slice.fromRaw` and `StringView.fromUtf8`.
- Scope: lib/core (`lib/core/cstr.cee`, one function; `src/library/sources.cee` regenerated);
  `spec/type-system.md` §22.3; `guide/unsafe-and-c.md`. No change to the checker, the backends, the runtime or the
  generated C, and no new diagnostic.

## 1. Context

`CStr<region r>` (ADR 0087 §2.4) is text with a NUL after it. It goes to C through `ptr()`. Nothing came back: for a
`*CChar` from C a program wrote

```ceesafe
let bytes = unsafe { Slice.fromRaw(raw as *u8, strlen(raw)) };
let text = StringView.fromUtf8(bytes) ?? "?";
```

with its own `extern "C"` declaration of `strlen`, which a freestanding program may not have, and with nothing that
handled a null pointer.

## 2. Decision

```ceesafe
public unsafe fn fromPtr(p: *CChar) -> CStr<r>?    // in lib/core's CStr<region r>
```

- **What.** The C string at `p`: the bytes up to the first NUL, counted by a loop in CeeSafe (no `strlen`, so it is
  the same in both profiles and in the interpreter). The result is a view of those bytes; nothing is copied.
- **Null.** A null `p` gives `null`. So do bytes that are not UTF-8, since a `CStr`'s `view()` is a `StringView`:
  what `StringView.fromUtf8` refuses, this refuses. A program that must tell the two apart tests `p.isNull()` first;
  one that wants the bytes whatever they are still has `Slice.fromRaw`.
- **Unsafe** (N0280 outside `unsafe`). The caller's contract: `p` is null, or points to bytes that end in a NUL and
  that are neither changed nor freed while the result, or a view taken from it, is used. As with `Slice.fromRaw`,
  the result holds no loan and its region is the one the context expects: the unsafe code bounds its use, typically
  by copying the text (`String.from(s.view())`) before C frees it.

Rejected: a safe function (nothing can check that the bytes end, or how long they live); a function on `StringView`
or `Slice` (the `CStr` gives both, with `view()` and `bytesWithNul()`, and keeps the NUL for handing the string back
to C); taking a length limit (that is `Slice.fromRaw` with `CStr.fromBytesWithNul`, which exist).

## 3. Compatibility

One new function of a library type: no existing program changes. A user type's own `fromPtr` is unaffected.

## 4. Tests

- `run-pass/cstr-from-ptr.cee`, compiled and in the interpreter (it calls no C): a literal's pointer gives its text,
  its length and the same address; an empty string; text that ends at the first of two NULs; bytes that are not UTF-8
  and a null pointer give `null`.
