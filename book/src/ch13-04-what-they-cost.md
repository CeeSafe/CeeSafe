---
title: What they cost
summary: What a closure and a for loop become in the generated C - an environment on the stack, a call that the compiler makes direct when it knows the closure, a counted loop without index checks - with measured times for direct and indirect calls and for the three lookfor versions.
assumes: [ch13-01-closures, ch13-02-iteration, ch13-03-project-improved]
keywords: [performance, cost, closure call, environment, specialisation, indirect call, emit c, explain-closure-calls, bounds check, benchmark]
specification: ["type-system.md §36.2", "optimisations.md §5", "optimisations.md §2"]
---

# What they cost

**This section teaches** what the compiler makes of a closure and of a `for` loop, how to look at it
yourself, and what the measurements say. It assumes [Closures](./ch13-01-closures) and
[Iteration](./ch13-02-iteration), and measures the programs of
[The project improved](./ch13-03-project-improved).

`ceesafec` compiles a program to C, and a C compiler makes the executable. So the cost of a feature can be
read: it is the C that the feature becomes. Every number below was measured on one machine (an Apple M1,
Apple clang 21.0.0, `ceesafec build` with its default options, the best of seven runs, on 13 November 2026).
Your numbers will differ; the ratios are the point.

## A closure in C

<span class="listing-caption"><strong>Listing 13-26</strong>, <code>main.cee</code>: A function that takes a closure, and a call that passes one capturing <code>limit</code>.</span>

```ceesafe
fn countIf(values: Slice<i32>, keep: borrow fn(i32) -> bool) -> i32 {
    var n = 0;
    for (v in values) {
        if (keep(v)) {
            n += 1;
        }
    }
    return n;
}

fn main() {
    let values = [3, 8, 1, 9, 4];
    let limit = 4;
    Console.writeLine(countIf(values[..], (v) => v > limit));
}
```

```text
2
```

`ceesafec build main.cee --emit c -o generated` writes the C to the directory `generated` instead of
compiling it. The program is in `generated/main.c`. The names in it are the compiler's own and change
between versions; the shape does not. Three pieces of it are the closure.

**The environment** is a struct with the address of the body's code, then one pointer for each captured
variable:

```c
struct ceeCE_997 {
    ceeCK_Q1i32bool f_0code;
    const int32_t * f_limit;
};
```

**The body** is an ordinary C function. Its first parameter is the environment, and it reads `limit` through
the pointer there:

```c
static bool ceeCB_997(const void *cee_env, CEE_UNUSED int32_t v_v_1) {
    CEE_UNUSED ceeCE_997 * v_env_0 = (ceeCE_997 *)(uintptr_t)cee_env;
    CEE_TEMP bool t_2 = false;
    CEE_TEMP bool t_3 = false;
    t_2 = (v_v_1 > (*(*v_env_0).f_limit));
    t_3 = t_2;
    return t_3;
}
```

**Making the closure**, in `main`, fills an environment that is a local of `main`, and takes its address.
That address is the closure's value:

```c
    t_3 = ceeCB_997;
    t_4 = &v_limit_1;
    t_5 = ((ceeCE_997){ .f_0code = t_3, .f_limit = t_4 });
    t_6 = &t_5.f_0code;
    t_7 = ceeXS1_ceeF_countIf(t_2, t_6);
```

So a closure that borrows costs two words on the stack here, 16 bytes, and no allocation. One more pointer
is added for each further capture. A closure that captures nothing has no environment: it is the function's
address. A `move` closure in a `Box` has the same environment, holding the values themselves, in one heap
allocation that `Box.create` makes.

::: tip Rule
A borrowing closure is a pointer to an environment on the stack: a code address and one pointer for each
capture. Making one allocates nothing.

*Specification: `type-system.md` §36.2, "C".*
:::

## The call: indirect, or direct

Inside `countIf`, the call `keep(v)` does not know which closure it has. In C it is a call through the
pointer:

```c
        t_7 = (*v_keep_1)(v_keep_1, v_v_3);
```

A C compiler cannot look into a call like that, so it cannot merge the body into the loop. But `main` does
know which closure it passes. So `ceesafec` makes a second copy of `countIf` for that call, a **variant**, in
which the parameter is bound to that closure's body. `main` calls the variant (`ceeXS1_ceeF_countIf` above),
and the call in it is direct:

```c
        t_7 = ceeCB_997(v_keep_1, v_v_3);
```

The C compiler can now put the comparison inside the loop. `ceesafec build --explain-closure-calls` reports
each call through a closure and what became of it:

```text
main.cee:4: in countIf: indirect: the closure is not known here
main.cee:4: in countIf#1: direct call to the closure at main.cee:14
variant countIf#1: parameter 1 = the closure at main.cee:14
```

The first line is the general `countIf`, which nothing calls in this program. The second is the variant.

::: tip Rule
A call through a closure becomes a direct call when the compiler knows the closure: a literal, or a named
function, made in the calling function and passed as an argument or kept in a local. It stays indirect when
the closure is read from a field or an element, is a `Box<dyn fn…>`, or depends on a branch.

*Specification: `optimisations.md` §5.1.*
:::

To measure the difference, the same `countIf` (returning a `u64`) is called a million times on an array of
1,024 numbers, with a closure that captures a limit which changes each round. A second program has the
comparison written in the loop, with no closure. The option `--no-closure-specialisation` turns the variants
off, so the first program can be built both ways.

| Program | Time |
|---|---:|
| the comparison written in the loop, no closure | 713 ms |
| the closure, as `ceesafec build` compiles it (a direct call) | 739 ms |
| the closure, built with `--no-closure-specialisation` (an indirect call) | 1,647 ms |

The closure costs nothing that these runs can show: a second set of runs gave 727 ms for the closure and
739 ms for the loop without one. The indirect call costs more than the whole loop, about a nanosecond for each of the 1,024
million calls, because the body here is one comparison. For the library's `sortBy`, the compiler's own
record gives the same picture: making the comparator's calls direct took 34% off a sorting benchmark with
clang and 28% with gcc (ADR 0019, `spec/decisions/0019-od9a-closure-call-specialisation.md`, §7).

## A loop in C

The `for` loop of `countIf` over a slice is a counter and a length that is read once:

```c
    t_4 = ((uint64_t)0);
    t_5 = v_values_0.len;
    for (;;) {
        t_6 = (t_4 < t_5);
        if (!t_6) {
            break;
        }
        v_v_3 = v_values_0.ptr[t_4];
        t_7 = (*v_keep_1)(v_keep_1, v_v_3);
        if (t_7) {
            v_n_2 = cee_add_i32(v_n_2, ((int32_t)1), &ceeL_0);
        }
        t_4 = ((uint64_t)(t_4 + ((uint64_t)1)));
    }
```

- The element is read with `ptr[t_4]` and no check. The counter is below the length by the loop's own test,
  so there is nothing to check.
- `n += 1` is `cee_add_i32`, which panics on overflow. Arithmetic is checked in a loop as anywhere.

A loop over indexes, `for (i in 0..values.length)` with `values[i]` in the body, has an index check in
principle. The compiler removes it when it can prove the index is in range, and
`ceesafec build --explain-bounds-checks` reports each check and whether it was removed. For the same counting
program written with indexes it prints `removed` for `values[i]`, and the times are the same:

| Loop | Time |
|---|---:|
| `for (v in values)` | 713 ms |
| `for (i in 0..values.length)` and `values[i]` | 716 ms |
| the same, built with `--no-bce` (every index check kept) | 745 ms |

A loop over an iterator calls `next` and tests the optional it returns. For the first loop of
[Listing 13-11](./ch13-02-iteration#writing-an-iterator), over a `Countdown`, the generated loop is:

```c
    for (;;) {
        t_3 = &v_iterator_0;
        t_2 = ceeM_9Countdown_next(t_3);
        t_4 = (uint32_t)t_2.tag;
        switch (t_4) {
        case ((uint32_t)1):
            v_n_1 = t_2.u.v_Some.f__0;
            ceesafe_write_u64((uint64_t)v_n_1);
            ceesafe_write_newline();
            continue;
        default:
            goto bb3;
        }
    }
```

The iterator is a local of the function, `next` is a direct call to a function in the same C file, and the
optional is a small struct returned by value. Nothing is allocated.

## The project, measured

The three versions of `lookfor` (chapter 12's, and the two steps of
[The project improved](./ch13-03-project-improved)) were built and run on a file of 3,000,000 lines and
104.5 MB, made by repeating `harbour.txt` 500,000 times. Each run looks for `tide`, finds 500,000 lines, and
writes them to `/dev/null`.

| Version | Time | Most memory in use |
|---|---:|---:|
| chapter 12: two functions, a vector | 183 ms | 116 MB |
| step 1: one function with a closure, a vector | 185 ms | 116 MB |
| step 2: an iterator, no vector | 181 ms | 108 MB |

- **The closure is free.** In step 1 the compiler knows each closure that `run` passes, and
  `--explain-closure-calls` reports `direct call to the closure` for each call in the variants of `search`.
- **The iterator saves the vector.** 500,000 views of 16 bytes are 8 MB. The rest of the memory is the file,
  which `readAll` still reads whole.
- **In step 2 the call is indirect**, and it does not show. `Matches` keeps the closure in a field, and the
  report says `in Matches.next: indirect: the closure is not known here`. Each call looks through one line of
  text, which costs far more than the call.

The times differ by less than they vary from run to run. The reasons for the changes of
[The project improved](./ch13-03-project-improved) are the duplicated code and the memory, not speed.

## Mistakes

::: warning Mistake: writing the loop by hand because a closure "must be slower"
A closure passed straight to a function is called directly, and measured the same as the hand-written loop
above. Check with `--explain-closure-calls` before you rewrite.
:::

::: warning Mistake: a stored closure in the innermost loop
A closure in a field, in a vector, or in a `Box<dyn fn…>` is called through a pointer. For a body of one
comparison that more than doubled the time above. If such a call is the hot part of a program, pass the
closure as a parameter of the function that loops, so that the compiler can make a variant.
:::

::: warning Mistake: timing with `ceesafec run --interp`
The interpreter is for checking what a program does. It does not run the C, so it says nothing about speed.
Measure the executable from `ceesafec build`.
:::

::: details Coming from Rust
Rust makes one copy of a generic function for each closure type it is called with. CeeSafe's closure
parameter has one type, `borrow fn(…)`, and the compiler makes the copies as an optimisation, for the calls
where it knows the closure. The result for a closure passed directly is the same: a direct call that can be
inlined. A closure in a field is like Rust's `&dyn Fn`: an indirect call.
:::

::: details Coming from C#, Java, TypeScript or Go
A closure here is not an object on the heap and a `for` loop makes no enumerator object. Neither gives a
garbage collector work, because there is none: the environment and the iterator are locals on the stack.
:::

## Summary

- A borrowing closure is an environment on the stack (a code address and a pointer for each capture) and a
  C function for its body. It allocates nothing.
- A call through a closure that the compiler knows becomes a direct call in a variant of the called function;
  `--explain-closure-calls` shows which. Measured, it costs the same as writing the code in the loop.
- A closure read from a field, an element or a box is an indirect call: about a nanosecond each on the
  machine used here, which matters only when the body is very small.
- `for` over a slice or vector is a counter and one length read, with no index check. An index loop's check
  is removed where the compiler can prove it (`--explain-bounds-checks`).
- `for` over an iterator is a call of `next` and a test for `null` each pass.
- `ceesafec build --emit c -o <directory>` shows the C for any program.

Next: chapter 14, More about packages.
