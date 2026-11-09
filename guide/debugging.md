# Debugging

A CeeSafe program is debugged as the C it compiles to, with the C mapped back to CeeSafe lines, so lldb and gdb work
without knowing anything about CeeSafe.

## A debug build

```sh
ceesafec build -g main.cee -o program
lldb ./program          # or: gdb ./program
```

`-g` writes `#line` directives into the C and compiles it with `-g -O0`. Breakpoints then go on CeeSafe lines, and
stepping moves through CeeSafe statements.

| | lldb | gdb |
|---|---|---|
| Stop at a line | `b main.cee:12` | `break main.cee:12` |
| Stop when a check fails, before the program exits | `b ceesafe_panic` | `break ceesafe_panic` |
| Where am I | `bt` | `bt` |

## Names

The C names follow fixed rules, so a CeeSafe name is easy to find:

| CeeSafe | C |
|---|---|
| function `length` | `ceeF_length` |
| method `Counter.bump` | `ceeM_7Counter_bump` (the type's name, prefixed by its length) |
| local or parameter `sum` | `v_sum_1` |
| a temporary the compiler made | `t_3` |
| field `x` | `f_x` |
| struct `Point` | `ceeT_Point` |
| an item of a dependency `geometry` | `ceeF_geometry_area`, `ceeT_geometry_Point` |

## CeeSafe names in the debugger

`editors/debug` adds two commands that print with CeeSafe names:

```sh
lldb ./program -o "command script import editors/debug/ceesafe_lldb.py"
gdb ./program -ex "source editors/debug/ceesafe_gdb.py"
```

- `ceesafe-locals`: the current frame's locals and parameters, without the compiler's temporaries, with struct fields by
  their CeeSafe names: `p = { x: 3, y: 4 }`.
- `ceesafe-bt`: the backtrace as `Counter.bump at main.cee:6`.
- `ceesafe-tasks`: in an async program, the thread's tasks and the line each one waits at, with a scope's children
  indented under the task that awaits the scope.

```text
(lldb) b main.cee:12
(lldb) run
(lldb) ceesafe-locals
p = { x: 3, y: 4 }
sum = 7
(lldb) ceesafe-bt
length at main.cee:12
main at main.cee:18
```

## Debug assertions

`debugAssert(condition)` and `debugAssert(condition, message)` check what should always hold, at no cost in a release
build: they are evaluated only in a debug build (`-g`), under `ceesafec test` and in the interpreter, and a release
build leaves them out entirely. `--debug-assertions` and `--no-debug-assertions` turn them on or off in any build.
A false one panics, as `assert` does.

```ceesafe
fn isSorted(values: Slice<u32>) -> bool {
    for (k in 1..values.length) {
        if (values[k - 1] > values[k]) {
            return false;
        }
    }
    return true;
}

fn main() {
    var scores = Vector<u32>.create();
    scores.push(3)!;
    scores.push(8)!;
    debugAssert(isSorted(scores.view()), "scores out of order");
    Console.writeLine(scores.length);
}
```

```text
2
```

Since a release build leaves the call out, the condition and message must not change anything that exists before
them, or the two builds would behave differently. The compiler checks this, through every function they call: no
assignment, `mut` argument, move or drop of an outer variable, no I/O, no C call, no call through a closure or `dyn`.
They may read, allocate, and change their own locals. To assert on something that changes, do the change first:

<!-- ceesafe: fail N0360 -->
```ceesafe
fn main() {
    var queue = Vector<u32>.create();
    queue.push(3)!;
    debugAssert(queue.pop() != null);
}
```

```text
main.cee:4:5: error[N0360]: 'debugAssert' must not change what exists before it, since release builds leave it out (ADR 0087 §2.5): this lends as 'mut' 'queue', which exists before the call
  |
4 |     debugAssert(queue.pop() != null);
  |     ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
```

Here, `let last = queue.pop(); debugAssert(last != null);` pops in every build. `assert` is always evaluated, so it
has no such rule.

## When the program panics

A failed check prints the reason and the CeeSafe location, and exits with status 134. Under a debugger, a breakpoint on
`ceesafe_panic` stops at the moment of the panic, with the whole call stack to inspect:

```text
(lldb) b ceesafe_panic
(lldb) run
(lldb) ceesafe-bt
ceesafe_panic at ceesafe_rt_core.c:207
cee_add_u8 at rt.h:190
grow at main.cee:2
main at main.cee:7
```

In an async program, stopped in a child task of a scope:

```text
(lldb) ceesafe-tasks
gather at main.cee:18 (waiting for its scope)
  async closure at main.cee:20 (running)
  async closure at main.cee:25 (waiting for a deadline)
```

Each line is the task's own async fn and the line where it last suspended. The running task is at the `await` it
resumed from, and `ceesafe-bt` shows where it is now.

For memory errors in `unsafe` code, `ceesafec run --interp` is often faster: the interpreter stops at the first invalid
access and names it.
