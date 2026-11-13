# Coming from another language

CeeSafe borrows deliberately from several languages, and each will feel partly familiar:

- its **syntax** from C# and TypeScript: braces, `fn name(x: T) -> R`, `let` and `var`, generics in `<…>`;
- its **ownership and borrowing** from Rust, with less annotation and a simpler checker;
- its **explicit allocators** from Zig, with use-after-free made a compile error;
- its **regions** from Cyclone, and its **compile-through-C** design and compiler-managed memory from Goose;
- its **deterministic destruction** from C++'s RAII, without the undefined behaviour.

These pages map what you know to CeeSafe, show the same program side by side, and point out the differences that will
trip you up.

| If you know | Read | The biggest changes |
|---|---|---|
| Rust | [CeeSafe for Rust programmers](rust.md) | no lifetime syntax on plain borrows; allocators per allocation; checks in every build; no macros or traits-as-types |
| Zig | [CeeSafe for Zig programmers](zig.md) | use-after-free and dangling pointers are compile errors; checks never turn off; generics instead of `comptime` |
| C or C++ | [CeeSafe for C and C++ programmers](c-and-cpp.md) | the compiler tracks ownership and borrows; no undefined behaviour; no preprocessor |
| C#, Java or TypeScript | [CeeSafe for C#, Java and TypeScript programmers](csharp-java-typescript.md) | no garbage collector, no classes or inheritance, no exceptions, no null references |
| Go | [CeeSafe for Go programmers](go.md) | no garbage collector; errors propagate with `?`; interfaces are explicit and static; no goroutines yet |

Why CeeSafe is built the way it is, against those languages, is on the repository's
[README](https://github.com/CeeSafe/CeeSafe#why-ceesafe).
