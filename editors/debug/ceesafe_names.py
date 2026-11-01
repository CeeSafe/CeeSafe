"""CeeSafe names for the C names ceesafec generates (spec/c-backend.md section 2.1, spec/tooling.md section 3.2).

Shared by ceesafe_lldb.py and ceesafe_gdb.py; plain Python, no debugger API. The C names are CeeSafe's
(Decision 0004): functions `ceeF_`, methods `ceeM_`, locals `v_`, fields `f_`.
"""

import os
import re

# Split C (ADR 0022): external names carry the program's name, `ceeF_<program>_main`. Set CEESAFE_PROGRAM to
# that name (ceesafec's --program, or the package's) to read them.
_PROGRAM = os.environ.get("CEESAFE_PROGRAM", "")

_LOCAL = re.compile(r"^v_(.+)_\d+$")
_FIELD = re.compile(r"^f_(.+)$")
_FUNCTION = re.compile(r"^ceeF_(.+)$")
_METHOD = re.compile(r"^ceeM_(\d+)(.+)$")


def local(name):
    """The CeeSafe name of a C local (`v_sum_1` is `sum`), or None for a compiler temporary or anything else."""
    match = _LOCAL.match(name or "")
    return match.group(1) if match else None


def field(name):
    """The CeeSafe name of a C struct member (`f_x` is `x`)."""
    match = _FIELD.match(name or "")
    return match.group(1) if match else name


def function(name):
    """The CeeSafe name of a C function: `ceeF_length` is `length`, `ceeM_7Counter_bump` is `Counter.bump`; an async
    fn's resume function `ceeAR_ceeF_twice` is `twice`, and an async closure's (`ceeAR_ceeCB_<n>`) `async closure`."""
    name = name or "?"
    if name.startswith("ceeAR_"):
        name = name[len("ceeAR_"):]
        if name.startswith("ceeCB_"):
            return "async closure"
    if _PROGRAM:
        for family in ("ceeF_", "ceeM_"):
            if name.startswith(family + _PROGRAM + "_"):
                name = family + name[len(family + _PROGRAM) + 1:]
    match = _FUNCTION.match(name)
    if match:
        return match.group(1)
    match = _METHOD.match(name)
    if match:
        length, rest = int(match.group(1)), match.group(2)
        if len(rest) > length + 1 and rest[length] == "_":
            return rest[:length] + "." + rest[length + 1:]
    return name
