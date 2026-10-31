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
