"""CeeSafe names in gdb (spec/tooling.md section 3.3): `source editors/debug/ceesafe_gdb.py`.

ceesafe-locals  the current frame's locals and parameters by their CeeSafe names, without the compiler's temporaries
ceesafe-bt      the backtrace as `name at file:line`
ceesafe-tasks   the stopped thread's async tasks (c-backend.md §3a): each at the line its async fn waits at, children
                indented under the task whose scope they are in
"""

import os
import sys

import gdb

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ceesafe_names  # noqa: E402


def _value(value, depth=0):
    """A value as CeeSafe would write it: a struct with its fields by their CeeSafe names, anything else as gdb shows it."""
    kind = value.type.strip_typedefs()
    if kind.code == gdb.TYPE_CODE_STRUCT and depth < 3:
        fields = [f"{ceesafe_names.field(f.name)}: {_value(value[f.name], depth + 1)}" for f in kind.fields() if f.name]
        return "{ " + ", ".join(fields) + " }"
    return str(value)


class CeeSafeLocals(gdb.Command):
    """The current frame's locals and parameters by their CeeSafe names."""

    def __init__(self):
        super().__init__("ceesafe-locals", gdb.COMMAND_DATA)

    def invoke(self, argument, from_tty):
        frame = gdb.selected_frame()
        block = frame.block()
        lines = []
        while block is not None:
            for symbol in block:
                name = ceesafe_names.local(symbol.name)
                if name is not None and (symbol.is_variable or symbol.is_argument):
                    lines.append(f"{name} = {_value(symbol.value(frame))}")
            if block.function is not None:
                break
            block = block.superblock
        for line in lines:
            gdb.write(line + "\n")


class CeeSafeBacktrace(gdb.Command):
    """The backtrace as `name at file:line`."""

    def __init__(self):
        super().__init__("ceesafe-bt", gdb.COMMAND_STACK)

    def invoke(self, argument, from_tty):
        frame = gdb.newest_frame()
        while frame is not None:
            sal = frame.find_sal()
            where = f"{os.path.basename(sal.symtab.filename)}:{sal.line}" if sal.symtab else "?"
            gdb.write(f"{ceesafe_names.function(frame.name())} at {where}\n")
            frame = frame.older()


class CeeSafeTasks(gdb.Command):
    """The stopped thread's async tasks: each at the line its async fn waits at, children under their scope's task."""

    def __init__(self):
        super().__init__("ceesafe-tasks", gdb.COMMAND_STACK)

    def invoke(self, argument, from_tty):
        try:
            text = gdb.parse_and_eval("(const char *)ceesafe_async_tasks()").string()
        except gdb.error:
            gdb.write("no async runtime in this program\n")
            return
        gdb.write((text.rstrip("\n") if text else "no tasks on this thread") + "\n")


CeeSafeLocals()
CeeSafeBacktrace()
CeeSafeTasks()
