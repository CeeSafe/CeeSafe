"""CeeSafe names in lldb (spec/tooling.md section 3.3): `command script import editors/debug/ceesafe_lldb.py`.

ceesafe-locals  the current frame's locals and parameters by their CeeSafe names, without the compiler's temporaries
ceesafe-bt      the backtrace as `name at file:line`
ceesafe-tasks   the stopped thread's async tasks (c-backend.md §3a): each at the line its async fn waits at, children
                indented under the task whose scope they are in

It also turns off lldb's summaries for Apple's C types (the `objc` category), which match C structs by name alone: a
dependency's `@repr(C)` struct keeps its declared C name (type-system.md §28.5), and a `struct Rect` or `struct Point`
of CeeSafe's showed "summary string parsing error" where its fields belong.
"""

import os
import sys

import lldb

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ceesafe_names  # noqa: E402


def _value(value, depth=0):
    """A value as CeeSafe would write it: a struct with its fields by their CeeSafe names, anything else as lldb shows it."""
    kind = value.GetType().GetCanonicalType().GetTypeClass()
    if kind in (lldb.eTypeClassStruct, lldb.eTypeClassClass) and depth < 3:
        fields = []
        for i in range(value.GetNumChildren()):
            child = value.GetChildAtIndex(i)
            fields.append(f"{ceesafe_names.field(child.GetName())}: {_value(child, depth + 1)}")
        return "{ " + ", ".join(fields) + " }"
    return value.GetValue() or value.GetSummary() or "?"


def _frame(debugger):
    return debugger.GetSelectedTarget().GetProcess().GetSelectedThread().GetSelectedFrame()


def ceesafe_locals(debugger, command, result, internal_dict):
    _quiet_apple_summaries(debugger)
    for variable in _frame(debugger).GetVariables(True, True, False, True):
        name = ceesafe_names.local(variable.GetName())
        if name is not None:
            result.AppendMessage(f"{name} = {_value(variable)}")


def ceesafe_bt(debugger, command, result, internal_dict):
    _quiet_apple_summaries(debugger)
    thread = debugger.GetSelectedTarget().GetProcess().GetSelectedThread()
    for frame in thread:
        entry = frame.GetLineEntry()
        where = f"{entry.GetFileSpec().GetFilename()}:{entry.GetLine()}" if entry.IsValid() else "?"
        result.AppendMessage(f"{ceesafe_names.function(frame.GetFunctionName())} at {where}")


def ceesafe_tasks(debugger, command, result, internal_dict):
    _quiet_apple_summaries(debugger)
    value = _frame(debugger).EvaluateExpression("(const char *)ceesafe_async_tasks()")
    address = value.GetValueAsUnsigned(0)
    if not value.GetError().Success() or address == 0:
        result.AppendMessage("no async runtime in this program")
        return
    error = lldb.SBError()
    text = debugger.GetSelectedTarget().GetProcess().ReadCStringFromMemory(address, 8192, error)
    result.AppendMessage(text.rstrip("\n") if error.Success() and text else "no tasks on this thread")


def _quiet_apple_summaries(debugger):
    """Turns off the `objc` language category, and any category named so. lldb makes it when a target is loaded:
    importing this script after the target (`lldb -o 'command script import …' -- program`) turns it off at once,
    and each command here turns it off again."""
    category = debugger.GetCategory(lldb.eLanguageTypeObjC)
    if category.IsValid():
        category.SetEnabled(False)
    debugger.HandleCommand("type category disable objc")


def __lldb_init_module(debugger, internal_dict):
    _quiet_apple_summaries(debugger)
    debugger.HandleCommand("command script add -f ceesafe_lldb.ceesafe_locals ceesafe-locals")
    debugger.HandleCommand("command script add -f ceesafe_lldb.ceesafe_bt ceesafe-bt")
    debugger.HandleCommand("command script add -f ceesafe_lldb.ceesafe_tasks ceesafe-tasks")
