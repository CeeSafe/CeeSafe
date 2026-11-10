"""CeeSafe names for the C names ceesafec generates (spec/c-backend.md section 2.1, spec/tooling.md section 3.2).

Shared by ceesafe_lldb.py and ceesafe_gdb.py; plain Python, no debugger API. The C names are CeeSafe's
(Decision 0004): functions `ceeF_`, methods `ceeM_`, generic methods' instances `ceeMG_` (ADR 0064), locals `v_`,
fields `f_`.
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
_GENERIC_METHOD = re.compile(r"^ceeMG_(\d+)(.+)$")
# A dependency's item (c-backend.md section 2.1): `3geo_area` alone, `03geo4Rect` after a length.
_QUALIFIED = re.compile(r"^([1-9]\d*)(.+)$")
_LENGTH_QUALIFIED = re.compile(r"^0([1-9]\d*)(.+)$")


def _qualified(name):
    """`3geo_area` is `geo.area`; a name without a package is itself."""
    match = _QUALIFIED.match(name)
    if match:
        length, rest = int(match.group(1)), match.group(2)
        if len(rest) > length + 1 and rest[length] == "_":
            return rest[:length] + "." + rest[length + 1:]
    return name


def _length_name(text):
    """The name at the start of `text` written with its length (`7Counter…`, or `03geo4Rect…` for a dependency's), and
    what follows it; or None."""
    match = _LENGTH_QUALIFIED.match(text)
    if match:
        package_length, rest = int(match.group(1)), match.group(2)
        package = rest[:package_length]
        inner = re.match(r"^(\d+)(.+)$", rest[package_length:])
        if len(package) == package_length and inner and len(inner.group(2)) >= int(inner.group(1)):
            length = int(inner.group(1))
            return package + "." + inner.group(2)[:length], inner.group(2)[length:]
        return None
    match = re.match(r"^(\d+)(.+)$", text)
    if match and len(match.group(2)) >= int(match.group(1)):
        length = int(match.group(1))
        return match.group(2)[:length], match.group(2)[length:]
    return None


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
        return _qualified(match.group(1))
    for family in ("ceeM_", "ceeMG_"):
        if name.startswith(family):
            # `ceeM_7Counter_bump`; `ceeMG_6Canvas_3add1S6Square`, the method's name by its length and the arguments
            # dropped; a dependency's owner `03geo4Rect`.
            owner = _length_name(name[len(family):])
            if owner and owner[1].startswith("_") and len(owner[1]) > 1:
                if family == "ceeM_":
                    return owner[0] + "." + owner[1][1:]
                method = _length_name(owner[1][1:])
                if method:
                    return owner[0] + "." + method[0]
    return name
