#!/usr/bin/env python3
"""Package tests for system libraries in the manifest (ADR 0047), on a copy of tests/packages in a temporary directory:

1. native-links: a C static library is built here (cc and ar, or cl and lib on Windows) into a directory named by
   $CEESAFE_TEST_LIBDIR; `wrap` links it, with its header from `include-dirs`, `mid` uses `wrap`, `app` uses `mid`.
   `ceesafec fetch` then `build` in app: the program prints 42 (2 × (20 + 1), the scale from the header).
2. `--emit c` writes ceesafe_link.txt with the include directory, the C file, `-L` and `-l` (cl's spelling with cl).
3. Without $CEESAFE_TEST_LIBDIR, `build` fails with N0321 naming it.
4. The package hash (what a git or registry dependency is locked by) changes when wrap's `links` does.
5. native-errors: `check` reports N0321 for each of its five invalid values.
6. impl-files: a method merged from an `impl` block in another file (ADR 0064 §1) panics at its own position.
7. impl-files: diagnostics in that method name its own file and line.
8. dependency-tests: `test` in app lists and runs app's test only, not the `@test` its dependency keeps in src/.
9. borrowed-listener: `test` of a library that only borrows AsyncTcpListener builds and passes (the C backend once
   panicked on it, then left the listener's drop undefined).
10. dependency-warnings: `check` in app reports app's N0400 and not its dependency's, and succeeds; with
    `--deny-warnings` it fails (ADR 0073 §2.4).
11. shadowing: a parameter, local, loop variable, closure parameter and binding named like the dependency `tls`
    (ADR 0074 §2.10) beside `tls.X` in types and outside their scopes, and associated constants through the package
    (`tls.TlsConfig.DEFAULT_PORT`, `tls.Version.COUNT`, ADR 0074 §2.7): `check` reports the two N0401 warnings only,
    and the program prints what it should, in the interpreter and compiled.
12. shadowing/item: an item named like the dependency is still N0315; shadowing/errors: another package's associated
    constant that is not public is N0312.
13. A `ceesafec` requirement in the manifest this ceesafec does not meet is N0321 (ADR 0076).
14. qualified-generics: `dep.f<T>(…)` and `dep.S<T>.m(…)` keep their type arguments.
15. c-names (ADR 0081 §2.2, §5): one C name declared by `sys` and again by a dependant is N0201 at the dependant's
    declaration, with a note at sys's and the hint to use `sys.strlen` (owned), or to make sys's declaration `public`
    (hidden); two files of one package give N0201 at the later file (two-files); `ceesafec fix` removes the
    declaration identical to sys's, with its doc lines and trailing comment, and an extern block it was alone in, and
    leaves one with another parameter type, result type or `@cname` (fix); with cc, mm and zz declaring one name and
    zz depending on cc, cc owns it for every order of app's `[dependencies]` (order, ADR 0081 §7); a dependant calls
    sys's `public` externs and a wrapper with a `borrow` parameter, in the interpreter and compiled (run).
16. same-names (ADR 0082 §2.1, §2.5): app, alpha and beta each have src/util.cee, and alpha has src/a/mod.cee and
    src/b/mod.cee. `build --split --emit c` writes one unit per file at c/<package>/<path>.c, each with its own
    functions, and names the dependencies' files by package in the location table; `build --split` with `--jobs 1`
    and `--jobs 4` makes programs that print the same, from the same C; an error in alpha's util.cee is reported as
    `../alpha/src/util.cee` with `"package": "alpha"`; two files whose units differ only in letter case are N0363
    (through two files outside the working directory, named by their file names, so that it runs on a file system that
    ignores case too; and in a package where the file system keeps `Util.cee` and `util.cee` apart), while two files
    whose units would be identical (two `util.cee` outside the working directory; `../x.cee` and `__/x.cee`) get
    `util-2.c` and build (ADR 0082 §7). Through a C compiler that logs its commands (POSIX), `--jobs 4` compiles each
    unit alone to its own object and links them; a failing compile exits 1 with the output in the units' order; a
    bad `--cflag`'s message is printed once with a count; `build --jobs` without `--split` is a usage error.
17. doc: `ceesafec doc` lists an interface's associated types and constants with its methods, in source order
    (ADR 0086 §2.4, §2.5).
18. embed (ADR 0087 §2.7, §2.8): app embeds from its own directory (a symbolic link inside it included, an empty
    file, its manifest) and joins its dependency words' constants with `+`; words embeds from its own directory. The
    program prints the same in the interpreter and compiled, and a file changed after `check` is what `build`
    compiles. escape gets N0362 for a symbolic link to a file outside the package, a path through a linked directory
    outside it, a link loop, `..` out of the package and a file of 16 MiB + 1; N0361 for `embedText` of Latin-1 bytes;
    and nothing for `embed` of the same bytes, an exactly 16 MiB file, or its manifest. The links and large files are
    made here (on Windows, without the privilege to make links, only what needs none).
19. embed in C and the interpreter (ADR 0087 §2.8, c-backend.md §2): a program embeds a 3 MiB file of random bytes
    (made here) through a static, a constant and a local, a 4-byte file and an empty one; it prints the lengths and a
    checksum, which Python computes, the same compiled and interpreted. `--emit c` holds each non-empty file's bytes
    once, as a `static const uint8_t` array, the short one too. With `--split`, each array is defined once, in
    ceesafe_instances.c, and declared `extern` in the header, and the program is about the size it is without.
20. [embed] roots (ADR 0087 §2.8, tooling.md §4.1), made here: app's roots are a directory outside it and a symbolic
    link inside it to another outside it; its path dependency near has a root outside it; its git dependency far
    embeds from itself. All three embed, and the program prints the files, in the interpreter. farbad, a git
    dependency whose roots are a committed symbolic link to a directory outside its checkout and `../shared`, gets
    N0362 for each (a git or registry package embeds only from itself). Invalid roots (absolute, `\`, `$`) and an
    unknown field in [embed] are N0321.
21. embed security (ADR 0087 §2.8, §9), against a secret file outside every package, made here:
    - evil, a git dependency, has a path dependency inner whose [embed] roots climb out of the checkout; evildeep
      reaches its deep the same way through two path dependencies. Neither reads the secret: N0362 at the embed, and
      `run --interp` does not print it (a package reached through a git or registry package embeds only from itself).
      evilout's path dependency leads out of its checkout, which `fetch` refuses (N0322, ADR 0152).
    - evilroots, a git dependency with roots outside its checkout, gets N0362; so does `check` run inside its checkout
      and the language server opening its file there (a package in the package cache embeds only from itself, even as
      the root package). evilout's path dependency lies outside the package cache (made here) and has roots outside
      it: N0362 with `check` run in evilout's checkout (its lock committed for that).
    - local: a symbolic link to the secret and a missing file get the same N0362 message, which names the path as
      written and neither the secret's directory nor its name; a FIFO is refused at once (not a regular file), and
      the language server answers with it there.
    - A constant `+` past 16 MiB is tests/reference/tests/corpus/check-fail/constant-concatenation-limit.cee.
22. hover: the language server shows the type of an `embedText` in a constant's value and of an `embed` in a static's.
23. The interface records' oracle (ADR 0082 §2.7, §5, §8.5): for every package of every fixture here, among them
    interface-records' (a dependency with a type error, one with an ownership error, a generic body, an `@effectsOf`
    function with a closure parameter, an async function, a coercion to `dyn`, effect chains through two
    dependencies) and transitive-failures' (a package with errors reached through an opaque package, through an
    ordinary one, through both, an opaque package with errors of its own, and two failures at different stages),
    `check` gives the same report cold, warm and with a `$CEESAFE_HOME` it cannot write to (no usable cache): the
    package's own diagnostics of `check --whole-program`, and for each package of its closure with errors in the
    whole program's check one N0364 at the manifest with a note, and nothing else of it. No record is made of a
    package with errors, nor any cache in the unusable home. `check --package nope` names the lock's packages.
24. check --all: one JSON document whose diagnostics are sorted by package, file and position, with a `packages`
    summary that counts them by their package; every package's own error found (borrowfail's too, which broken's
    type error stops in the first round), no N0364; the text in the same order; the same cold and warm; its exit
    status. `check --jobs` is a usage error: check is one process. A dependency's failing test hides none of its
    dependents' own errors (all-tests); N0400 and N0401 in a dependency's own files are reported by `check --all`
    and fail it with `--deny-warnings`, but not by `check` in its dependant, cold or warm, and the dependency's
    record counts them (all-lints).
25. Stale records are never used (ADR 0082 §2.7): a record of mid forged to say its functions have no effects is used
    while it holds (app's N0295 change), but not with another banner, after its package's sources change, after a
    dependency's interface changes, when it is writable by the group or others, when it is cut short, when it is a
    symbolic link, when the cache directory is writable by others, or when the records directory is a symbolic link;
    a change inside a body dependents skip, and comment lines added at the top of base, keep base's interface, so
    mid's record still holds. `build` reads no record: it checks the whole program.
26. The cache: `$CEESAFE_HOME/cache` and its `records` directory are made with mode 0700, and each record 0600.
27. repr-dep (type-system.md §28.5; ADR 0081 §8): geo declares `@repr(C)` structs (one nested, one with an array
    member), a `@repr(u8)` enum, exports and an exported constant that use them, and ordinary items; other declares
    ordinary items with geo's names, and app declares `geo_Plain` and `geo_twice` (c-backend.md §2.1). app uses them in its own exports, in generic instances and through
    c/caller.c, which includes ceesafe_exports.h and calls both packages' exports. Built as one file, `--split` and
    `--emit obj` (linked here with cc), the program prints the same; `--emit c` spells the types by their declared
    names (`struct Point`, `Shade`), never `geo.`. clash: app's `@repr(C)` struct named as its dependency geo's, and
    rival's struct and enum named as geo's (rival and geo unrelated, geo first by name), are N0201 with a note at geo's;
    the per-package check's N0364 for rival names geo and suggests `check --whole-program`, since `check --package
    rival` alone finds nothing.
28. A C++ caller (ADR 0088 §2.6): a C++11 file includes repr-dep's ceesafe_exports.h (its `extern "C"` guards),
    calls both packages' exports with their `@repr` types, compiles with -Wall -Werror and links with the program,
    which prints the same.
29. qualified-impls (ADR 0097): `impl dep.I for T`, `impl dep.G<u8> for T` and `impl Local for dep.T`, beside an impl
    through an import, called directly, through a bound and through `dyn`: `check` is clean and the program prints
    what it should, in the interpreter and compiled. `impl dep.I for dep.T` is the orphan rule's N0267, an interface
    the dependency lacks is N0311, and a type parameter named like the dependency is still N0315.
30. constant-other-file: a static's initialiser calls a generic `@constant` function of another file whose body is
    not a static initialiser for that call: one N0338, in the function's file at its line (the report once
    panicked, reading the calling file's text at the other file's offset).
34. impl-bounds-order (ADR 0130 §2.6): an instance of a dependency's struct or enum has the functions of the bounded
    `impl` blocks whose bounds its argument meets, by an impl declared later in the file, in another file of the
    package and in the dependency, for a bound that is an interface, `Copy` and `Send`: `check` is clean and the
    program prints what it should, in the interpreter and compiled; the unmet cases are N0268, and N0334 for Send.
35. region-bounds (type-system.md §25.8, ADR 0134): a dependency's functions, struct and method whose region
    parameters have bounds. Used while every borrow is alive, `check` is clean and the program prints what it
    should, in the interpreter and compiled. A function that forwards its regions without restating the bound is
    N0245, and a caller that keeps what the bound lets the callee return or store after the referent is destroyed
    is N0243, with a note that names the callee and its bound.
31. private-items (ADR 0109): a private function, generic function, struct, generic struct, enum, interface, constant,
    static, method, static function and associated constant of src/shapes.cee, and an `impl` block's of
    src/more.cee (a struct's and an enum's), named in src/main.cee in a call, a value, a type, a bound, `dyn`, an `impl` header, a literal and
    a pattern: N0296 at each marked line and nowhere else, the message naming the declaring file; a private field
    and a private struct in a struct pattern too, and a struct with a drop block taken apart outside its file
    (N0252, ADR 0123 §2.6). Without those
    lines the program runs: a generic of shapes.cee instantiated from main.cee, values of private types held there,
    and locals named like private items are all allowed.
36. qualified-literals (type-system.md §2): shapes and boxes both have `Item` and `Pair<T>`, and app imports both.
    Struct literals named with their package, `shapes.Item { … }` and `boxes.Pair<u32> { … }`, as a value, an
    argument and a field: `check` is clean (0.44.0 gave N0313 at the literal's `Item`, and a parse error for the
    generic one) and the program prints what it should, in the interpreter and compiled. The simple names in a
    literal are still N0313.
37. impl-statics (type-system.md §2, cee-proxy's F16): `impl Make for Thing` in app, its functions without
    `public`; make's generic `build<T: Make>` calls the static `T.make` through the bound: `check` is clean (0.48.2
    gave N0222, "'Thing.make' is not public") and the program prints 42 and 4, in the interpreter and compiled.
38. impl-order (type-system.md §2, ClayWind's finding K3): things declares `public struct Thing<T>` and `public enum
    Shape` in src/thing.cee and adds their methods in `impl` blocks in src/additions.cee, a file that sorts before it.
    app imports both: `check` is clean (0.49.0 took the first impl as the type's declaration, which is private, and
    gave N0312 at each import) and the program prints 42 and 12, in the interpreter and compiled.
39. windowed (ADR 0151, tooling.md §4.1): `windowed = true` checks and builds; the program prints what it should
    through a pipe (on Windows a redirected stream keeps its handle), and its C `main` calls `ceesafe_rt_windowed()`
    first, which a build without the field does not; `--windowed` gives the same C. On Windows the executable's PE
    subsystem is 2 (GUI) with the field, alone and with `--split`, and 3 (console) without, and the test program of
    a windowed package is still 3. `windowed` on a library, an object, a freestanding package or in [target.windows],
    and a value that is not a boolean, are one N0321 each; `--windowed` with `--emit obj`, `--emit lib` or
    `--profile freestanding`, and in a freestanding package, is a usage error (status 2).
40. interface-scope (type-system.md §2, ClayWind's finding M3): shapes' `Source.drag` returns shapes' `Drag?`. app
    imports `Source` and `Point`, not `Drag`, and writes `Drag?` in a field and in its impl of `drag`: `check` gives
    N0204 at those two places alone (0.53.0 gave a third, at the offset of shapes' `Drag` read in app's file, since
    the interface's signature was resolved in the impl's file). mine declares a `Drag` of its own and returns it from
    `drag`: N0267 alone (0.53.0 accepted it, taking the interface's `Drag` for mine's). imported imports `Drag`:
    `check` is clean and the program prints 42, in the interpreter and compiled.
41. git-subdirectories (ADR 0152, tooling.md §4.1–§4.3, §4.6–§4.8), a repository made here (pushed to a bare one,
    tagged v1 to v5): shapes and extra in subdirectories, extra naming shapes `{ path = "../shapes" }`, shapes
    embedding from the repository's assets/. app names both by `git`, `tag` and `dir`: `fetch` locks two packages (one
    shapes, no N0326), each with its `dir`; `check` is clean and the program prints 6 and the embedded word, in the
    interpreter and compiled; the language server and `check` in shapes' directory of the checkout find nothing (its
    embed from assets/ is inside the checkout); `check --offline` and `fetch --offline` work with the remote gone; the
    lock's `dir` spelled otherwise (`shapes/../shapes`) is the same, another `dir` is N0324.
    A `dir` that is absolute, has `\`, `$` or a leading `~`, is empty or leaves the checkout, and `dir` on a path
    dependency, are N0321; a directory without a manifest, a path dependency out of the checkout (v3), and a symbolic
    link leading out of it (v4, POSIX) are N0322; shapes at v1 and extra at v2, whose path names shapes2, are N0326
    naming both directories. `publish` in a subdirectory writes `dir`, `index-check` accepts the entry, and an
    application using it from that index runs, its lock recording `dir`.
42. dependency-patch (ADR 0174, tooling.md §4.1, §4.2): app depends on extra and shapes by path, extra on shapes by
    another path: `fetch` is N0326. With `[patch] shapes = { path = "../shapes-local" }`, `fetch` locks one shapes,
    from the patch, `check` is clean and the program prints the patched package's values, in the interpreter and
    compiled. A patch that names no package of the build, and one given as a version requirement, are N0321; a
    dependency's own [patch] is not read.
43. manifest-frameworks (ADR 0184): see manifest_frameworks.
44. library-build-default (ADR 0187, tooling.md §4.1, CeeUsb's finding F13): `build` with no `--emit` in a package of
    kind "library" writes its static library out/<name>.a and ceesafe/embed.h beside it, with no N0300; `--emit exe`
    there is still N0300.
45. polls (ADR 0190, tooling.md §4.1, §12.1): with `polls = true` in the root's [package], `build` makes a program
    whose `Async.runBound` answers true and whose bound ends an isolated child that spins; without the key, with
    `polls = false` or with `--no-polls` it answers false, and `--polls` turns them on without the key. A
    dependency's `polls = true` changes nothing for a root without it. `test` builds its harness with them: a test of
    `Async.runBound` passes with the key and fails with `--no-polls`. `--emit c`'s C files each start with the
    `#define CEE_POLLS 1` line and, compiled with no flag (POSIX), give the same program. `polls` that is not a
    boolean, in a [target.OS] table or in a freestanding package is one N0321; `--polls` with `--no-polls`,
    `--no-polls` with `--cflag -DCEE_POLLS=1`, and `--polls` with `--profile freestanding` are usage errors.
46. file-private-names (ADR 0230, type-system.md §2, the SQLite port's finding 15): src/printf.cee and
    src/datetime.cee each declare private `appendInteger`, a generic `twice`, a struct `Row` with a method and an
    `impl Shape for Row`, an enum `Mode` with an `impl Mode` block, an interface `Shape` used as `dyn`, a constant
    and a static; printf.cee's private `Layout` and `label` hide lib/core's `Layout` and main.cee's `label` there
    only. `check` is clean and the program prints 1148 and 74 (worked out in main.cee), in the interpreter and
    compiled, with each private item's C name marked by its file. A fifth file naming `appendInteger` gets N0296
    with the file it is private to, and its unknown type `Rwo` no "did you mean 'Row'"; the language server's
    definition of `appendInteger` in datetime.cee is datetime.cee's.

usage: python3 tools/package_tests.py --compiler out/ceesafec
"""

import argparse
import itertools
import json
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
WINDOWS = os.name == "nt"


def run(argv, cwd, env=None):
    e = dict(os.environ)
    e.pop("CEESAFE_TEST_LIBDIR", None)
    if env:
        e.update(env)
    return subprocess.run(argv, cwd=cwd, env=e, capture_output=True, text=True)


def build_library(work):
    lib = os.path.join(work, "lib")
    os.makedirs(lib)
    source = os.path.join(work, "native-links", "clib", "mylib.c")
    include = os.path.join(work, "native-links", "clib", "include")
    if WINDOWS:
        steps = [["cl", "/nologo", "/c", "/I", include, source, "/Fo:" + os.path.join(lib, "mylib.obj")],
                 ["lib", "/nologo", "/OUT:" + os.path.join(lib, "mylib.lib"), os.path.join(lib, "mylib.obj")]]
    else:
        cc = os.environ.get("CC", "cc")
        steps = [[cc, "-c", "-I", include, source, "-o", os.path.join(lib, "mylib.o")],
                 ["ar", "rcs", os.path.join(lib, "libmylib.a"), os.path.join(lib, "mylib.o")]]
    for step in steps:
        p = run(step, work)
        if p.returncode != 0:
            sys.exit(f"package_tests: cannot build the C library: {' '.join(step)}\n{p.stdout}{p.stderr}")
    return lib


def language_server(compiler, path, hover=None, method="textDocument/hover"):
    """What `ceesafec lsp` answers for the file at `path` once opened: the diagnostics it publishes, as JSON text, and
    with `hover` (a line and a character) the answer of `method` there (the hover's, or another position request's,
    such as a definition's); both empty on a timeout (a FIFO that hung the check would be one)."""
    with open(path) as f:
        text = f.read()
    uri = "file://" + ("/" if not path.startswith("/") else "") + path.replace(chr(92), "/")
    messages = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"capabilities": {}}},
        {"jsonrpc": "2.0", "method": "initialized", "params": {}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
            "uri": uri, "languageId": "ceesafe", "version": 1, "text": text}}},
    ]
    if hover:
        messages.append({"jsonrpc": "2.0", "id": 7, "method": method, "params": {
            "textDocument": {"uri": uri}, "position": {"line": hover[0], "character": hover[1]}}})
    messages += [{"jsonrpc": "2.0", "id": 2, "method": "shutdown"}, {"jsonrpc": "2.0", "method": "exit"}]
    data = b""
    for m in messages:
        body = json.dumps(m).encode()
        data += b"Content-Length: %d\r\n\r\n" % len(body) + body
    try:
        p = subprocess.run([compiler, "lsp"], input=data, capture_output=True, timeout=60)
    except subprocess.TimeoutExpired:
        return "", ""
    parts = p.stdout.decode("utf-8", "replace").split("Content-Length:")
    published = "".join(part for part in parts if "publishDiagnostics" in part)
    answered = ""
    for part in parts:
        body = part.split("\r\n\r\n", 1)[-1]
        try:
            message = json.loads(body)
        except ValueError:
            continue
        if message.get("id") == 7:
            answered = json.dumps(message.get("result"))
    return published, answered


def language_server_diagnostics(compiler, path):
    return language_server(compiler, path)[0]


def record_fields(data):
    """An interface record's fields (src/host/record_cache.cee): each `<length>:<bytes>\\n`."""
    fields, i = [], 0
    while i < len(data):
        j = data.index(b":", i)
        n = int(data[i:j])
        fields.append(data[j + 1:j + 1 + n])
        if data[j + 1 + n:j + 2 + n] != b"\n":
            raise ValueError("not a record")
        i = j + 2 + n
    return fields


def record_bytes(fields):
    """The record of `fields`, its last field (the SHA-256 of the rest) made afresh."""
    import hashlib
    body = b"".join(b"%d:%s\n" % (len(f), f) for f in fields[:-1])
    digest = hashlib.sha256(body).hexdigest().encode()
    return body + b"%d:%s\n" % (len(digest), digest)


def json_check(compiler, directory, *extra, env=None):
    """`check --diagnostics-format=json` in `directory`: its exit status and document (None if it printed none)."""
    p = run([compiler, "check", "--diagnostics-format=json", *extra], directory, env)
    try:
        return p.returncode, json.loads(p.stdout)
    except ValueError:
        return p.returncode, None


def oracle_problem(directory, whole, per):
    """What makes `per`, the per-package check, differ from `whole`, the whole-program check (ADR 0082 §2.7,
    tooling.md §4.4): the package's own diagnostics must be the same; every other package with errors in the whole
    program's check must be one N0364 at the package's manifest, with a note at its first error, and nothing else of
    it (its warnings neither); or None."""
    root = None
    with open(os.path.join(directory, "ceesafe.toml")) as f:
        for line in f:
            if line.strip().startswith("name"):
                root = line.split("=", 1)[1].strip().strip('"')
                break

    def own(diagnostics):
        return [d for d in diagnostics if d.get("package", root) == root and d["code"] != "N0364"]

    failed = {d["package"] for d in whole["diagnostics"]
              if d["severity"] == "error" and d.get("package", root) != root}
    if own(per["diagnostics"]) != own(whole["diagnostics"]):
        return (f"diagnostics differ:\n    whole {json.dumps(own(whole['diagnostics']))[:900]}"
                f"\n    per   {json.dumps(own(per['diagnostics']))[:900]}")
    others = [d for d in per["diagnostics"] if d.get("package", root) != root]
    if others:
        return f"a dependency's own diagnostics are reported: {json.dumps(others)[:600]}"
    n0364 = [d for d in per["diagnostics"] if d["code"] == "N0364"]
    named = []
    for d in n0364:
        name = d["message"].split("the dependency '", 1)[-1].split("'", 1)[0]
        named.append(name)
        if d.get("package") != root or d["file"] != "ceesafe.toml" or d["severity"] != "error" or not d["notes"]:
            return f"N0364 is not an error at the manifest with a note: {d}"
        if f"--package {name}'" not in d["message"] or (
                "--whole-program'" not in d["message"] and f"'ceesafec check --package {name}' to see" not in d["message"]):
            return f"N0364 does not say how to see {name}'s errors: {d['message']}"
    if sorted(named) != sorted(failed):
        return f"N0364 names {sorted(named)}, but the packages with errors are {sorted(failed)}"
    return None


def impl_bounds_order(compiler, work, problems):
    """Whether an instance meets a bounded `impl` block's bounds is decided once the whole program's impls, fields
    and Copy types are known (ADR 0130 §2.6): the met cases run, the unmet ones are N0268 (N0334 for Send)."""
    root = os.path.join(work, "impl-bounds-order")
    shutil.copytree(os.path.join(ROOT, "tests", "packages", "impl-bounds-order"), root)
    app = os.path.join(root, "app")
    expected = "7\n30\n3\n5\n3\n5\n7\n9"
    p = run([compiler, "check", "--diagnostics-format=json"], app)
    try:
        found = [d["code"] for d in json.loads(p.stdout)["diagnostics"]]
    except (ValueError, KeyError):
        found = [p.stdout[:300] + p.stderr[:300]]
    if p.returncode != 0 or found:
        problems.append(f"impl-bounds-order: check should be clean: {p.returncode} {found}")
    p = run([compiler, "run", "--interp"], app)
    if p.returncode != 0 or p.stdout.strip() != expected:
        problems.append(f"impl-bounds-order: run --interp printed {p.stdout.strip()!r} ({p.returncode}) "
                        f"{p.stderr[-300:]}")
    p = run([compiler, "build"], app)
    exe = os.path.join(app, "out", "app.exe" if WINDOWS else "app")
    if p.returncode != 0:
        problems.append(f"impl-bounds-order: build failed: {p.stderr.strip()[-600:]}")
    else:
        out = run([exe], app).stdout.strip()
        if out != expected:
            problems.append(f"impl-bounds-order: the program printed {out!r}")
    p = run([compiler, "check", "--diagnostics-format=json"], os.path.join(root, "unmet"))
    try:
        found = [(d["code"], d["start"]["line"]) for d in json.loads(p.stdout)["diagnostics"]]
    except (ValueError, KeyError):
        found = []
    wanted = [("N0268", 7), ("N0268", 12), ("N0268", 14), ("N0334", 16), ("N0268", 18)]
    if p.returncode != 1 or sorted(found, key=lambda d: d[1]) != wanted:
        problems.append(f"impl-bounds-order/unmet: expected {wanted}: {p.returncode} {found} {p.stderr[:300]}")


def region_bounds(compiler, work, problems):
    """A bound on a region parameter of a dependency's function, struct or method (ADR 0134) is the declaration's
    flow, so a caller in another package is held to it; the diagnostic names the bound."""
    root = os.path.join(work, "region-bounds")
    shutil.copytree(os.path.join(ROOT, "tests", "packages", "region-bounds"), root)
    app = os.path.join(root, "app")
    expected = "short\nlong\n2\nshort\nlong"
    p = run([compiler, "check", "--diagnostics-format=json"], app)
    try:
        found = [d["code"] for d in json.loads(p.stdout)["diagnostics"]]
    except (ValueError, KeyError):
        found = [p.stdout[:300] + p.stderr[:300]]
    if p.returncode != 0 or found:
        problems.append(f"region-bounds: check should be clean: {p.returncode} {found}")
    p = run([compiler, "run", "--interp"], app)
    if p.returncode != 0 or p.stdout.strip() != expected:
        problems.append(f"region-bounds: run --interp printed {p.stdout.strip()!r} ({p.returncode}) "
                        f"{p.stderr[-300:]}")
    p = run([compiler, "build"], app)
    exe = os.path.join(app, "out", "app.exe" if WINDOWS else "app")
    if p.returncode != 0:
        problems.append(f"region-bounds: build failed: {p.stderr.strip()[-600:]}")
    else:
        out = run([exe], app).stdout.strip()
        if out != expected:
            problems.append(f"region-bounds: the program printed {out!r}")
    p = run([compiler, "check", "--diagnostics-format=json"], os.path.join(root, "misuse"))
    try:
        diagnostics = json.loads(p.stdout)["diagnostics"]
    except (ValueError, KeyError):
        diagnostics = []
    found = [(d["code"], d["start"]["line"]) for d in diagnostics]
    wanted = [("N0245", 6), ("N0243", 13), ("N0243", 23), ("N0243", 32), ("N0243", 42)]
    if p.returncode != 1 or sorted(found, key=lambda d: d[1]) != wanted:
        problems.append(f"region-bounds/misuse: expected {wanted}: {p.returncode} {found} {p.stderr[:300]}")
    notes = [n["message"] for d in diagnostics for n in d.get("notes", [])]
    for text in ("'views.pick' declares 'region b: a' ('b' outlives 'a'), so its result may hold the borrow passed here",
                 "'views.keep' declares 'region b: a' ('b' outlives 'a'), so it may store the borrow passed here "
                 "through another argument",
                 "declares 'region input: scratch' ('input' outlives 'scratch')",
                 "declares 'region l: scratch' ('l' outlives 'scratch')"):
        if not any(text in note for note in notes):
            problems.append(f"region-bounds/misuse: no note says {text!r}: {notes}")


def qualified_literals(compiler, work, problems):
    """A struct literal named with its package (`dep.Item { … }`, `dep.Pair<u32> { … }`) is not the simple name two
    imports provide (N0313), which a literal without the package still is."""
    root = os.path.join(work, "qualified-literals")
    shutil.copytree(os.path.join(ROOT, "tests", "packages", "qualified-literals"), root)
    app = os.path.join(root, "app")
    expected = "21\n10\n11"
    status, document = json_check(compiler, app)
    found = [d["code"] for d in document["diagnostics"]] if document else ["no document"]
    if status != 0 or found:
        problems.append(f"qualified-literals: check should be clean: {status} {found}")
    p = run([compiler, "run", "--interp"], app)
    if p.returncode != 0 or p.stdout.strip() != expected:
        problems.append(f"qualified-literals: run --interp printed {p.stdout.strip()!r} ({p.returncode}) "
                        f"{p.stderr[-300:]}")
    p = run([compiler, "build"], app)
    exe = os.path.join(app, "out", "app.exe" if WINDOWS else "app")
    if p.returncode != 0:
        problems.append(f"qualified-literals: build failed: {p.stderr.strip()[-600:]}")
    else:
        out = run([exe], app).stdout.strip()
        if out != expected:
            problems.append(f"qualified-literals: the program printed {out!r}")
    status, document = json_check(compiler, os.path.join(root, "ambiguous"))
    found = [(d["code"], d["start"]["line"]) for d in document["diagnostics"]] if document else []
    if status != 1 or found != [("N0313", 6), ("N0313", 7)]:
        problems.append(f"qualified-literals/ambiguous: expected N0313 at lines 6 and 7: {status} {found}")


def impl_statics(compiler, work, problems):
    """An interface's static function is as visible as the interface: called through a bound from the interface's
    package, an impl's function written without `public` checks (cee-proxy's F16)."""
    root = os.path.join(work, "impl-statics")
    shutil.copytree(os.path.join(ROOT, "tests", "packages", "impl-statics"), root)
    app = os.path.join(root, "app")
    status, document = json_check(compiler, app)
    found = [d["code"] for d in document["diagnostics"]] if document else ["no document"]
    if status != 0 or found:
        problems.append(f"impl-statics: check should be clean: {status} {found}")
    p = run([compiler, "run", "--interp"], app)
    if p.returncode != 0 or p.stdout.split() != ["42", "4"]:
        problems.append(f"impl-statics: run --interp printed {p.stdout.strip()!r} ({p.returncode}) {p.stderr[-300:]}")
    p = run([compiler, "build"], app)
    exe = os.path.join(app, "out", "app.exe" if WINDOWS else "app")
    if p.returncode != 0:
        problems.append(f"impl-statics: build failed: {p.stderr.strip()[-600:]}")
    else:
        out = run([exe], app).stdout.split()
        if out != ["42", "4"]:
            problems.append(f"impl-statics: the program printed {out!r}")


def impl_order(compiler, work, problems):
    """An inherent impl declares no name: in a file that sorts before its type's, it leaves the type public (ClayWind's
    finding K3)."""
    root = os.path.join(work, "impl-order")
    shutil.copytree(os.path.join(ROOT, "tests", "packages", "impl-order"), root)
    app = os.path.join(root, "app")
    status, document = json_check(compiler, app)
    found = [d["code"] for d in document["diagnostics"]] if document else ["no document"]
    if status != 0 or found:
        problems.append(f"impl-order: check should be clean: {status} {found}")
    p = run([compiler, "run", "--interp"], app)
    if p.returncode != 0 or p.stdout.split() != ["42", "12"]:
        problems.append(f"impl-order: run --interp printed {p.stdout.strip()!r} ({p.returncode}) {p.stderr[-300:]}")
    p = run([compiler, "build"], app)
    exe = os.path.join(app, "out", "app.exe" if WINDOWS else "app")
    if p.returncode != 0:
        problems.append(f"impl-order: build failed: {p.stderr.strip()[-600:]}")
    else:
        out = run([exe], app).stdout.split()
        if out != ["42", "12"]:
            problems.append(f"impl-order: the program printed {out!r}")


def interface_scope(compiler, work, problems):
    """An interface's signature names types in the interface's own file, whichever package implements it (ClayWind's
    finding M3)."""
    root = os.path.join(work, "interface-scope")
    shutil.copytree(os.path.join(ROOT, "tests", "packages", "interface-scope"), root)
    expected = {
        "app": [("N0204", "src/main.cee", 11, 12), ("N0204", "src/main.cee", 24, 29)],
        "mine": [("N0267", "src/main.cee", 13, 8)],
        "imported": [],
    }
    for name, wanted in expected.items():
        status, document = json_check(compiler, os.path.join(root, name))
        found = sorted((d["code"], d["file"], d["start"]["line"], d["start"]["column"])
                       for d in document["diagnostics"]) if document else ["no document"]
        if found != sorted(wanted) or (status == 0) != (not wanted):
            problems.append(f"interface-scope/{name}: expected {wanted}: {status} {found}")
    imported = os.path.join(root, "imported")
    p = run([compiler, "run", "--interp"], imported)
    if p.returncode != 0 or p.stdout.split() != ["42"]:
        problems.append(f"interface-scope: run --interp printed {p.stdout.strip()!r} ({p.returncode}) "
                        f"{p.stderr[-300:]}")
    p = run([compiler, "build"], imported)
    exe = os.path.join(imported, "out", "imported.exe" if WINDOWS else "imported")
    if p.returncode != 0:
        problems.append(f"interface-scope: build failed: {p.stderr.strip()[-600:]}")
    else:
        out = run([exe], imported).stdout.split()
        if out != ["42"]:
            problems.append(f"interface-scope: the program printed {out!r}")


def check_all_rounds(compiler, work, problems):
    """Check 24's further cases, on packages made here: a dependency's failing test hides none of its dependents' own
    errors; N0400 and N0401 are found in every package's own files by `check --all` (and fail it with
    `--deny-warnings`), while `check` in a dependant reports neither; a record counts its package's own warnings."""
    def write(path, text):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as handle:
            handle.write(text)

    def manifest(name, kind, dependencies=()):
        deps = "".join(f'{d} = {{ path = "../{d}" }}\n' for d in dependencies)
        return (f'[package]\nname = "{name}"\nversion = "0.1.0"\nkind = "{kind}"\n'
                + (f"\n[dependencies]\n{deps}" if deps else ""))

    allocates = "    var v = Vector<u64>.create();\n    v.push(1)!;\n    return v.length as u64"
    tests = os.path.join(work, "all-tests")
    write(os.path.join(tests, "l", "ceesafe.toml"), manifest("l", "library"))
    write(os.path.join(tests, "l", "src", "lib.cee"), "public fn one() -> u64 {\n    return 1;\n}\n")
    write(os.path.join(tests, "l", "tests", "t.cee"),
          "@test\nfn broken() -> Result<void, TestFailure> {\n    let x: u64 = \"s\";\n    return Ok();\n}\n")
    write(os.path.join(tests, "r", "ceesafe.toml"), manifest("r", "library", ["l"]))
    write(os.path.join(tests, "r", "src", "lib.cee"),
          f"@effects()\npublic fn pure() -> u64 {{\n{allocates} + l.one();\n}}\n")
    write(os.path.join(tests, "app", "ceesafe.toml"), manifest("app", "executable", ["l", "r"]))
    write(os.path.join(tests, "app", "src", "main.cee"),
          f"@effects()\nfn mine() -> u64 {{\n{allocates};\n}}\n\nfn main() {{\n"
          "    Console.writeLine(mine() + r.pure());\n}\n")
    env = {"CEESAFE_HOME": os.path.join(work, "all-tests-home")}
    app = os.path.join(tests, "app")
    run([compiler, "fetch", "--offline"], app, env)
    p = run([compiler, "check", "--all", "--diagnostics-format=json"], app, env)
    try:
        document = json.loads(p.stdout)
        found = sorted((d.get("package", ""), d["code"]) for d in document["diagnostics"])
    except (ValueError, KeyError) as e:
        found = f"no document ({e}): {p.stdout[:200]} {p.stderr[:200]}"
    want = [("app", "N0295"), ("l", "N0205"), ("r", "N0295")]
    if found != want or p.returncode != 1:
        problems.append(f"check --all: l's failing test hid its dependents' own errors: {found} ({p.returncode})")

    lints = os.path.join(work, "all-lints")
    shutil.copytree(os.path.join(ROOT, "tests", "packages", "shadowing", "tls"), os.path.join(lints, "tls"))
    write(os.path.join(lints, "mid", "ceesafe.toml"), manifest("mid", "library", ["tls"]))
    write(os.path.join(lints, "mid", "src", "lib.cee"),
          "public fn describe(tls: tls.TlsConfig) -> u32 {\n    return tls.port + 1;\n}\n\n"
          "public fn greet(name: StringView) {\n    Console.writeLine(\"hello {name}\");\n}\n")
    write(os.path.join(lints, "top", "ceesafe.toml"), manifest("top", "executable", ["mid", "tls"]))
    write(os.path.join(lints, "top", "src", "main.cee"),
          "fn main() {\n    mid.greet(\"x\");\n"
          "    Console.writeLine(mid.describe(tls.TlsConfig { port: 1 }));\n}\n")
    home = os.path.join(work, "all-lints-home")
    env = {"CEESAFE_HOME": home}
    top = os.path.join(lints, "top")
    run([compiler, "fetch", "--offline"], top, env)
    for attempt in ("cold", "warm"):
        p = run([compiler, "check", "--diagnostics-format=json"], top, env)
        if p.returncode != 0 or '"diagnostics":[]' not in p.stdout.replace(" ", ""):
            problems.append(f"check ({attempt}): a dependency's N0400 or N0401 reported: {p.stdout[:300]}")
        p = run([compiler, "check", "--deny-warnings"], top, env)
        if p.returncode != 0:
            problems.append(f"check --deny-warnings ({attempt}): a dependency's warning failed it: {p.stderr[:300]}")
    records = os.path.join(home, "cache", "records")
    mids = [n for n in os.listdir(records) if n.startswith("mid-")] if os.path.isdir(records) else []
    if os.name != "nt" and not sys.platform.startswith("freebsd"):  # no records on Windows or FreeBSD (ADR 0149)
        counts = [int(record_fields(open(os.path.join(records, n), "rb").read())[7]) for n in mids]
        if not counts or any(c != 2 for c in counts):
            problems.append(f"records: mid's record does not count its two warnings: {counts}")
    for attempt in ("warm", "again"):
        p = run([compiler, "check", "--all", "--diagnostics-format=json"], top, env)
        try:
            document = json.loads(p.stdout)
            found = sorted((d.get("package", ""), d["code"], d["severity"]) for d in document["diagnostics"])
        except (ValueError, KeyError) as e:
            found = f"no document ({e}): {p.stdout[:200]} {p.stderr[:200]}"
        if found != [("mid", "N0400", "warning"), ("mid", "N0401", "warning")] or p.returncode != 0:
            problems.append(f"check --all ({attempt}): expected mid's N0400 and N0401: {found} ({p.returncode})")
        p = run([compiler, "check", "--all", "--deny-warnings"], top, env)
        if p.returncode != 1 or "N0400" not in p.stderr or "N0401" not in p.stderr:
            problems.append(f"check --all --deny-warnings ({attempt}): a dependency's warnings did not fail it "
                            f"({p.returncode}) {p.stderr[-300:]}")


def interface_records(compiler, work, problems):
    """Checks 23-26 (ADR 0082 §2.7), on their own copy of the fixtures and their own $CEESAFE_HOME."""
    oracle = os.path.join(work, "oracle")
    shutil.copytree(os.path.join(ROOT, "tests", "packages"), oracle)
    home = os.path.join(work, "records-home")
    env = {"CEESAFE_HOME": home}
    # A cache that cannot be used: its home is not writable, so no record is read or made.
    unusable = os.path.join(work, "records-home-readonly")
    os.makedirs(unusable)
    directories = sorted(top for top, _, files in os.walk(oracle) if "ceesafe.toml" in files)
    for directory in directories:
        run([compiler, "fetch", "--offline"], directory, env)
    os.chmod(unusable, 0o555)
    env_unusable = {"CEESAFE_HOME": unusable}

    # 23. Every package: the per-package check, cold then warm, and with no usable cache, against the whole program's.
    compared = 0
    for directory in directories:
        name = os.path.relpath(directory, oracle)
        status_whole, whole = json_check(compiler, directory, "--whole-program", env=env)
        if status_whole not in (0, 1) or whole is None:
            continue
        cold = json_check(compiler, directory, env=env)
        warm = json_check(compiler, directory, env=env)
        none = json_check(compiler, directory, env=env_unusable)
        if cold != warm:
            problems.append(f"records: {name}: the warm check differs from the cold one")
        if cold != none:
            problems.append(f"records: {name}: the check without a usable cache differs from the cold one")
        if cold[0] != status_whole or cold[1] is None:
            problems.append(f"records: {name}: exit {cold[0]}, the whole program's {status_whole}")
            continue
        problem = oracle_problem(directory, whole, cold[1])
        if problem:
            problems.append(f"records: {name}: {problem}")
        compared += 1
    os.chmod(unusable, 0o755)
    if compared < 50:
        problems.append(f"records: only {compared} packages compared")
    if os.path.isdir(os.path.join(unusable, "cache")):
        problems.append("records: a cache was made in a home that is not writable")
    fixture = os.path.join(oracle, "interface-records")
    cache = os.path.join(home, "cache", "records")
    names = sorted(os.listdir(cache)) if os.path.isdir(cache) else []
    for package in ("base", "mid"):
        if not any(n.startswith(package + "-") and n.endswith(".rec") for n in names):
            problems.append(f"records: no record of {package}: {names[:20]}")
    for package in ("broken", "borrowfail"):  # a package with errors is never taken from a record: none is made
        if any(n.startswith(package + "-") for n in names):
            problems.append(f"records: a record of {package}, which has errors")
    # usesbroken: broken's type error stops the check before the ownership checks, as the whole program's does, so
    # borrowfail's error is not found there (`check --package borrowfail` finds it).
    expected = {
        "app": ["N0295"] * 7,
        "usesbroken": ["N0205", "N0364"],
        "usesborrow": ["N0364"],
    }
    for package, codes in expected.items():
        _, document = json_check(compiler, os.path.join(fixture, package), env=env)
        found = [d["code"] for d in document["diagnostics"]] if document else None
        if found != codes:
            problems.append(f"records: {package}: expected {codes}, found {found}")
        if package == "app" and document and not any(
                "which calls 'base.helper', which does console I/O" in d["message"] for d in document["diagnostics"]):
            problems.append("records: app: no chain through base's private helper, named 'base.helper'")
    transitive = os.path.join(oracle, "transitive-failures")
    for package, failed in (("viaopaque", "deep"), ("viaplain", "deep"), ("mixed", "deep")):
        _, document = json_check(compiler, os.path.join(transitive, package), env=env)
        n0364 = [d for d in document["diagnostics"] if d["code"] == "N0364"] if document else []
        if len(n0364) != 1 or f"the dependency '{failed}'" not in n0364[0]["message"]:
            problems.append(f"records: {package}: expected one N0364 for {failed}: {n0364}")
    p = run([compiler, "check", "--package", "nope"], os.path.join(fixture, "app"), env)
    if p.returncode != 2 or "there is no package 'nope'" not in p.stderr or "packages: app, base, mid" not in p.stderr or \
            "usage:" in p.stderr:
        problems.append(f"check --package nope: {p.returncode} {p.stderr[:300]}")

    # 24. check --all: sorted, with a summary by the diagnostics' packages, each package's own errors found; the text
    # in the same order; the same cold and warm.
    usesbroken = os.path.join(fixture, "usesbroken")
    env_all = {"CEESAFE_HOME": os.path.join(work, "all-home")}
    p1 = run([compiler, "check", "--all", "--diagnostics-format=json"], usesbroken, env_all)
    p2 = run([compiler, "check", "--all", "--diagnostics-format=json"], usesbroken, env_all)
    try:
        document = json.loads(p1.stdout)
        keys = [(d.get("package", ""), d["file"], d["start"]["line"], d["start"]["column"])
                for d in document["diagnostics"]]
        summary = {e["name"]: (e["errors"], e["warnings"]) for e in document["packages"]}
        counted = {}
        for d in document["diagnostics"]:
            e, w = counted.get(d["package"], (0, 0))
            counted[d["package"]] = (e + 1, w) if d["severity"] == "error" else (e, w + 1)
        want = {"base": (0, 0), "borrowfail": (1, 0), "broken": (1, 0), "mid": (0, 0), "usesbroken": (1, 0)}
        if keys != sorted(keys) or summary != want or document["errors"] != 3 or p1.returncode != 1:
            problems.append(f"check --all: {keys} {summary} {document['errors']} ({p1.returncode})")
        if any(counted.get(n, (0, 0)) != v for n, v in summary.items()):
            problems.append(f"check --all: the summary is not the diagnostics' count: {summary} {counted}")
        if any(d["code"] == "N0364" for d in document["diagnostics"]):
            problems.append("check --all: an N0364 for a package whose own diagnostics are in the report")
        if [e["name"] for e in document["packages"]] != sorted(want):
            problems.append(f"check --all: the summary is not by name: {document['packages']}")
        text = run([compiler, "check", "--all"], usesbroken, env_all)
        shown = [line.split(": ")[0] + ":" + line.split("[")[1].split("]")[0] for line in text.stderr.splitlines()
                 if ": error[" in line or ": warning[" in line]
        listed = [f"{d['file']}:{d['start']['line']}:{d['start']['column']}:{d['code']}"
                  for d in document["diagnostics"]]
        if shown != listed:
            problems.append(f"check --all: the text's order is not the JSON's: {shown} {listed}")
        if text.returncode != 1 or "ceesafec: package 'broken': 1 error, 0 warnings" not in text.stderr:
            problems.append(f"check --all as text: {text.stderr[-500:]} ({text.returncode})")
    except (ValueError, KeyError, IndexError) as e:
        problems.append(f"check --all printed no document ({e}): {p1.stdout[:300]} {p1.stderr[:300]}")
    if p1.stdout != p2.stdout:
        problems.append("check --all: cold and warm differ")
    p = run([compiler, "check", "--all"], os.path.join(fixture, "base"), env)
    if p.returncode != 0 or p.stdout or p.stderr:
        problems.append(f"check --all in base: ({p.returncode}) {p.stdout[:200]} {p.stderr[:200]}")
    p = run([compiler, "check", "--all", "--whole-program"], usesbroken, env)
    if p.returncode != 2:
        problems.append(f"check --all --whole-program is not a usage error ({p.returncode})")
    p = run([compiler, "check", "--all", "--jobs", "2"], usesbroken, env)
    if p.returncode != 2:
        problems.append(f"check --jobs is not a usage error ({p.returncode}): check runs in one process")

    # 25. Stale records. A fresh cache, then a forged record of mid whose functions have no effects: app's N0295 that
    # go through mid change while it is used.
    stale = os.path.join(work, "stale")
    shutil.copytree(fixture, stale)
    home2 = os.path.join(work, "stale-home")
    env2 = {"CEESAFE_HOME": home2}
    app = os.path.join(stale, "app")
    for package in ("mid", "app"):
        run([compiler, "fetch", "--offline"], os.path.join(stale, package), env2)
    records = os.path.join(home2, "cache", "records")

    def codes():
        _, document = json_check(compiler, app, env=env2)
        return [d["code"] + d["message"] for d in document["diagnostics"]] if document else None

    clean = codes()

    def record_of(package):
        found = [n for n in os.listdir(records) if n.startswith(package + "-") and n.endswith(".rec")]
        return os.path.join(records, found[0]) if len(found) == 1 else None

    def forge(package):
        """Rewrites the package's current record to say its functions have no effects; returns its path."""
        for name in os.listdir(records):  # the current record alone: made afresh
            if name.startswith(package + "-"):
                os.remove(os.path.join(records, name))
        codes()
        path = record_of(package)
        if path is None:
            return None
        fields = record_fields(open(path, "rb").read())[:-1]
        head, rest = fields[:12], fields[12:]
        out, k = [], 0
        for _ in range(int(head[11])):
            out += [rest[k], rest[k + 1], b"0"]
            k += 3
            for _ in range(4):
                k += 1 + int(rest[k]) + 3
                out += [b"0", b"", b"0", b"0"]
        with open(path, "wb") as f:
            f.write(record_bytes(head + out + [b""]))
        os.chmod(path, 0o600)
        return path

    def forged_used(label, expect_used, prepare=None, package="mid"):
        path = forge(package)
        if path is None:
            problems.append(f"stale records: {label}: no record of {package}")
            return
        if prepare:
            prepare(path)
        found = codes()
        used = found is not None and found != clean
        if used != expect_used:
            problems.append(f"stale records: {label}: the forged record was {'' if used else 'not '}used: {found}")
        # Whatever happened, a clean check again: a fresh record replaces the forged one.
        for name in os.listdir(records):
            if name.startswith(package + "-"):
                os.remove(os.path.join(records, name))
        codes()

    if clean is None or record_of("mid") is None:
        problems.append(f"stale records: the first check: {clean}")
    else:
        forged_used("a valid record", True)

        def other_banner(path):
            fields = record_fields(open(path, "rb").read())
            fields[3] = b"ceesafec 0.0.1"
            with open(path, "wb") as f:
                f.write(record_bytes(fields))

        forged_used("another banner", False, other_banner)
        forged_used("writable by others", False, lambda path: os.chmod(path, 0o666))
        forged_used("writable by the group", False, lambda path: os.chmod(path, 0o620))
        forged_used("cut short", False, lambda path: open(path, "r+b").truncate(os.path.getsize(path) - 10))

        def linked(path):
            # The record a symbolic link to the forged one, kept elsewhere: openat never follows it.
            elsewhere = os.path.join(work, "linked.rec")
            shutil.move(path, elsewhere)
            os.symlink(elsewhere, path)

        forged_used("a symbolic link", False, linked)
        mid_source = os.path.join(stale, "mid", "src", "lib.cee")
        original = open(mid_source).read()

        def edit_mid(path):
            with open(mid_source, "a") as f:
                f.write("\n// a comment changes the sources\n")

        forged_used("mid's sources changed", False, edit_mid)
        with open(mid_source, "w") as f:
            f.write(original)
        codes()
        base_source = os.path.join(stale, "base", "src", "lib.cee")
        base_original = open(base_source).read()

        def edit_base_interface(path):
            with open(base_source, "a") as f:
                f.write("\npublic fn extra() -> u64 => 1;\n")

        forged_used("base's interface changed", False, edit_base_interface)
        with open(base_source, "w") as f:
            f.write(base_original)
        codes()

        def record_base():
            # base's record afresh, from its own check: mid's key is then known without checking mid.
            p = run([compiler, "check", "--package", "base", "--record"], app, env2)
            if p.returncode != 0 or not p.stdout.startswith("24:ceesafe-interface-record"):
                problems.append(f"stale records: check --package base --record: ({p.returncode}) {p.stderr[:300]}")

        def edit_base_body(path):
            # Inside grow's body, which dependents skip: base's interface, and so mid's key, stay.
            with open(base_source, "w") as f:
                f.write(base_original.replace("v.push(n)!;", "v.push(n + 0)!;"))
            record_base()

        forged_used("a change inside base's skipped body", True, edit_base_body)
        with open(base_source, "w") as f:
            f.write(base_original)
        codes()

        def comment_base(path):
            # Lines added above everything: the interface hash holds no position, so mid's key stays.
            with open(base_source, "w") as f:
                f.write("// a comment\n// and another\n\n" + base_original)
            record_base()

        forged_used("a comment at the top of base", True, comment_base)
        with open(base_source, "w") as f:
            f.write(base_original)
        codes()
        forged_used("the cache writable by others", False, lambda path: os.chmod(records, 0o777))
        os.chmod(records, 0o700)
        # build reads no record: a forged one changes nothing.
        forge("mid")
        p = run([compiler, "build", "--emit", "c", "-o", os.path.join(work, "stale-c")], app, env2)
        if p.returncode != 1 or p.stderr.count("error[N0295]") != 7:
            problems.append(f"stale records: build used a record: ({p.returncode}) {p.stderr[:400]}")
        # The records directory replaced by a symbolic link to another: never used.
        moved = os.path.join(work, "moved-records")
        os.rename(records, moved)
        os.symlink(moved, records)
        found = codes()
        if found != clean or len(os.listdir(moved)) == 0:
            problems.append(f"stale records: a linked records directory: {found}")
        os.remove(records)
        os.rename(moved, records)

    # The compiler is never found by the name it was started with: a `ceesafec` first on PATH, the program's argv[0]
    # (`exec -a ceesafec`), neither runs nor changes the records' key (a record of the real compiler's is used).
    if not WINDOWS:
        fake = os.path.join(work, "fakebin")
        os.makedirs(fake)
        marker = os.path.join(work, "fake-ran")
        with open(os.path.join(fake, "ceesafec"), "w") as f:
            f.write(f"#!/bin/sh\ntouch '{marker}'\nexit 0\n")
        os.chmod(os.path.join(fake, "ceesafec"), 0o755)
        e = dict(os.environ, PATH=fake + os.pathsep + os.environ.get("PATH", ""), CEESAFE_HOME=home2)
        settled = codes()
        before = sorted(os.listdir(records))
        p = subprocess.run(["ceesafec", "check", "--diagnostics-format=json"], executable=compiler, cwd=app, env=e,
                           capture_output=True, text=True)
        try:
            found = [d["code"] + d["message"] for d in json.loads(p.stdout)["diagnostics"]]
        except (ValueError, KeyError):
            found = None
        after = sorted(os.listdir(records))
        if os.path.exists(marker) or found != settled or after != before:
            problems.append(f"records: started as 'ceesafec' with another first on PATH: ran it "
                            f"{os.path.exists(marker)}, the same report {found == settled}, new records "
                            f"{sorted(set(after) - set(before))}")

    # 26. The cache's modes.
    for path, mode in ((os.path.join(home2, "cache"), 0o700), (records, 0o700)):
        if os.path.isdir(path) and (os.stat(path).st_mode & 0o777) != mode:
            problems.append(f"records: {path} has mode {oct(os.stat(path).st_mode & 0o777)}, not {oct(mode)}")
    for name in os.listdir(records) if os.path.isdir(records) else []:
        if (os.stat(os.path.join(records, name)).st_mode & 0o777) != 0o600:
            problems.append(f"records: {name} has mode {oct(os.stat(os.path.join(records, name)).st_mode & 0o777)}")


def header_include_dirs(compiler, work, problems):
    """33. header-include-dirs (ADR 0120): meter binds C through `@header("meter.h")`, a header in its `include-dirs`;
    app depends on it. The generated C finds the header in the plain build, `--split`, `--emit obj` and `--emit lib`,
    and the programs print 15. `--emit c` to a relative directory writes ceesafe_link.txt there."""
    base = os.path.join(work, "header-include-dirs")
    shutil.copytree(os.path.join(ROOT, "tests", "packages", "header-include-dirs"), base)
    app = os.path.join(base, "app")
    suffix = ".exe" if WINDOWS else ""
    for label, extra in (("build", []), ("build --split", ["--split"])):
        exe = os.path.join(app, "out", "app-" + str(len(extra)) + suffix)
        p = run([compiler, "build", "-o", exe] + extra, app)
        if p.returncode != 0:
            problems.append(f"header-include-dirs: {label} failed: {p.stderr.strip()[-600:]}")
            continue
        out = run([exe], app).stdout.strip()
        if out != "15":
            problems.append(f"header-include-dirs: {label}: the program printed {out!r}, not 15")
    if not WINDOWS:
        for emit, output in (("obj", "app.o"), ("lib", "app.a")):
            p = run([compiler, "build", "--emit", emit, "-o", os.path.join(app, "out", emit, output)], app)
            if p.returncode != 0:
                problems.append(f"header-include-dirs: --emit {emit} failed: {p.stderr.strip()[-600:]}")
    p = run([compiler, "build", "--emit", "c", "-o", "c-out"], app)
    link = os.path.join(app, "c-out", "ceesafe_link.txt")
    if p.returncode != 0 or not os.path.exists(link):
        problems.append(f"header-include-dirs: --emit c -o c-out wrote no c-out/ceesafe_link.txt: "
                        f"{p.returncode} {p.stderr.strip()[-300:]}")


def driver_messages(compiler, work, problems):
    """32. What the driver says: help for a command, an environment failure without the usage text, a lock written
    with another index, and the places of `publish`'s and `index-check`'s errors."""
    base = os.path.join(work, "driver-messages")
    tally, index, user = (os.path.join(base, name) for name in ("tally", "index", "user"))
    remote = os.path.join(base, "remote.git")

    def write(path, text):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)

    def place(output):
        """The first diagnostic's `file:line:column: error[code]`."""
        first = output.split("\n")[0]
        return first.split("]")[0] + "]" if "]" in first else first

    # `<command> --help` is the usage text, with status 0; after `--` it is the program's own argument.
    write(os.path.join(user, "ceesafe.toml"), '[package]\nname = "user"\n\n[dependencies]\ntally = "^0.1"\n')
    write(os.path.join(user, "src", "main.cee"),
          'fn main() {\n    Console.writeLine(Process.argument(1) ?? "none");\n}\n')
    for command in ("build", "check", "run", "fetch", "fmt", "cbind"):   # cbind prints its own (tooling.md §11)
        for flag in ("--help", "-h"):
            p = run([compiler, command, flag], user)
            if p.returncode != 0 or not p.stdout.startswith("usage: ceesafec") or p.stderr:
                problems.append(f"help: '{command} {flag}': {p.returncode} {p.stdout[:80]!r} {p.stderr[:200]!r}")
    p = run([compiler, "--help"], user)
    if "default: out/<name> in a package" not in p.stdout:
        problems.append("help: -o's default in a package is out/<name>")
    single = os.path.join(base, "single.cee")
    write(single, 'fn main() {\n    Console.writeLine(Process.argument(1) ?? "none");\n}\n')
    p = run([compiler, "run", "--interp", single, "--", "--help"], base)
    if p.returncode != 0 or p.stdout.strip() != "--help":
        problems.append(f"help: '--help' after '--' is the program's: {p.returncode} {p.stdout!r} {p.stderr[:200]!r}")

    # A C compiler that cannot be run is not a usage error's page of options: one line.
    p = run([compiler, "build", single, "--cc", "no-such-cc-gaps2", "-o", os.path.join(base, "single")], base)
    if p.returncode == 0 or "cannot run the C compiler 'no-such-cc-gaps2'" not in p.stderr or "usage:" in p.stderr:
        problems.append(f"--cc: a compiler that cannot be run: {p.returncode} {p.stderr[:300]!r}")
    p = run([compiler, "build", single, "--no-such-option"], base)
    if p.returncode != 2 or "usage:" not in p.stderr:
        problems.append(f"an unknown option is a usage error with the usage text: {p.returncode} {p.stderr[:200]!r}")

    # A library published to an index of its own.
    git = ["git", "-c", "user.name=test", "-c", "user.email=test@localhost"]
    library = '[package]\nname = "tally"\nversion = "0.1.0"\nkind = "library"\n'
    write(os.path.join(tally, "ceesafe.toml"), library)
    write(os.path.join(tally, "src", "lib.cee"), "public fn two() -> i32 => 2;\n")
    os.makedirs(remote)
    run(["git", "init", "-q", "--bare"], remote)
    for argv in (["init", "-q"], ["add", "-A"], ["commit", "-qm", "one"], ["remote", "add", "origin", remote],
                 ["push", "-q", "origin", "HEAD:main"], ["fetch", "-q", "origin"]):
        run(git + argv, tally)
    p = run([compiler, "publish"], tally)
    if p.returncode != 0 or not p.stdout.startswith("[[version]]"):
        problems.append(f"publish: {p.returncode} {p.stdout[:200]!r} {p.stderr[:300]!r}")
        return
    entry = p.stdout
    write(os.path.join(index, "packages", "tally.toml"), entry)
    for argv in (["init", "-q"], ["add", "-A"], ["commit", "-qm", "one"]):
        run(git + argv, index)

    # A lock written with CEESAFE_INDEX set, read without it: N0324 names both indexes and the variable.
    p = run([compiler, "fetch"], user, {"CEESAFE_INDEX": index})
    q = run([compiler, "check"], user, {"CEESAFE_INDEX": index})
    if p.returncode != 0 or q.returncode != 0:
        problems.append(f"index: fetch and check with CEESAFE_INDEX: {p.stderr[:300]!r} {q.stdout[:300]!r}")
    e = dict(os.environ)
    e.pop("CEESAFE_INDEX", None)
    p = subprocess.run([compiler, "check"], cwd=user, env=e, capture_output=True, text=True)
    first = p.stderr.split("\n")[0]
    if (p.returncode != 1 or place(p.stderr) != "ceesafe.toml:5:1: error[N0324]" or "CEESAFE_INDEX" not in first
            or index.replace("\\", "/") not in first or "CeeSafe-index" not in first):
        problems.append(f"index: a lock from another index: {p.returncode} {p.stderr[:500]!r}")

    # index-check: a version that is not newer is reported at its own [[version]].
    write(os.path.join(index, "packages", "tally.toml"), entry + entry)
    p = run([compiler, "index-check", "."], index)
    expected = f"index:packages/tally.toml:{len(entry.splitlines()) + 1}:1: error[N0328]"
    if p.returncode != 1 or place(p.stderr) != expected or "is not newer than" not in p.stderr:
        problems.append(f"index-check: a duplicate version at its entry ({expected}): {p.returncode} {p.stderr[:300]!r}")

    # publish: N0335 at the dependency, at `kind`, at [package].
    for text, expected in (
            (library + '\n[dependencies]\nuser = { path = "../user" }\n', "ceesafe.toml:7:1"),
            (library.replace("library", "executable"), "ceesafe.toml:4:8"),
            ('# a comment\n[package]\nname = "tally"\nkind = "library"\n', "ceesafe.toml:2:1")):
        write(os.path.join(tally, "ceesafe.toml"), text)
        p = run([compiler, "publish"], tally)
        if p.returncode != 1 or place(p.stderr) != expected + ": error[N0335]":
            problems.append(f"publish: N0335 at {expected}: {p.returncode} {p.stderr[:300]!r}")


def git_subdirectories(compiler, work, problems):
    """41. Packages in a repository's subdirectories (ADR 0152): `dir`, a path dependency inside a checkout, the lock,
    embedding from the checkout, the registry."""
    base = os.path.join(work, "git-subdirectories")
    repository, remote, app = (os.path.join(base, name) for name in ("repository", "remote.git", "app"))
    git = ["git", "-c", "user.name=test", "-c", "user.email=test@localhost"]

    def write(path, text):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)

    def read(path):
        return open(path, encoding="utf-8").read() if os.path.exists(path) else ""

    def codes(p):
        try:
            return [d["code"] for d in json.loads(p.stdout)["diagnostics"]]
        except (ValueError, KeyError):
            return [p.stdout[:300] + p.stderr[:300]]

    def fetch_codes(p):
        """The codes of `fetch`'s diagnostics (it prints them as text)."""
        return re.findall(r"error\[(N[0-9]{4})\]", p.stderr)

    def commit_and_tag(tag):
        for argv in (["add", "-A"], ["commit", "-qm", tag], ["tag", tag], ["push", "-q", "origin", "HEAD:main", tag]):
            run(git + argv, repository)

    # The repository: shapes, and extra, which names shapes by a path, and assets beside them; v1. At v2 extra names
    # a copy of shapes in another directory; at v3, a path out of the checkout.
    write(os.path.join(repository, "assets", "word.txt"), "shared")
    write(os.path.join(repository, "shapes", "ceesafe.toml"),
          '[package]\nname = "shapes"\nkind = "library"\n\n[embed]\nroots = ["../assets"]\n')
    write(os.path.join(repository, "shapes", "src", "lib.cee"),
          'public const WORD: StringView = embedText("../../assets/word.txt");\n\npublic fn two() -> i32 => 2;\n')
    write(os.path.join(repository, "extra", "ceesafe.toml"),
          '[package]\nname = "extra"\nkind = "library"\n\n[dependencies]\nshapes = { path = "../shapes" }\n')
    write(os.path.join(repository, "extra", "src", "lib.cee"), "public fn four() -> i32 => shapes.two() * 2;\n")
    os.makedirs(remote)
    run(["git", "init", "-q", "--bare"], remote)
    run(git + ["init", "-q"], repository)
    run(git + ["remote", "add", "origin", remote], repository)
    commit_and_tag("v1")
    shutil.copytree(os.path.join(repository, "shapes"), os.path.join(repository, "shapes2"))
    write(os.path.join(repository, "extra", "ceesafe.toml"),
          '[package]\nname = "extra"\nkind = "library"\n\n[dependencies]\nshapes = { path = "../shapes2" }\n')
    commit_and_tag("v2")
    write(os.path.join(repository, "extra", "ceesafe.toml"),
          '[package]\nname = "extra"\nkind = "library"\n\n[dependencies]\nshapes = { path = "../../outside" }\n')
    commit_and_tag("v3")
    url = remote.replace("\\", "/")

    def application(directory, dependencies, main=None):
        write(os.path.join(directory, "ceesafe.toml"), '[package]\nname = "app"\n\n[dependencies]\n' + dependencies)
        write(os.path.join(directory, "src", "main.cee"), main or
              "fn main() {\n    Console.writeLine(extra.four() + shapes.two());\n    Console.writeLine(shapes.WORD);\n}\n")
        return directory

    # Both packages by git, tag and dir: one shapes (no N0326), the lock records the directories, and everything works.
    application(app, f'shapes = {{ git = "{url}", tag = "v1", dir = "shapes" }}\n'
                     f'extra = {{ git = "{url}", tag = "v1", dir = "./extra/" }}\n')
    p = run([compiler, "fetch"], app)
    lock = read(os.path.join(app, "ceesafe.lock"))
    if p.returncode != 0 or lock.count("[[package]]") != 2 or 'dir = "shapes"' not in lock or 'dir = "extra"' not in lock:
        problems.append(f"git dir: fetch: {p.returncode} {p.stderr[:400]!r} lock {lock!r}")
    p = run([compiler, "check", "--diagnostics-format=json"], app)
    if p.returncode != 0 or codes(p):
        problems.append(f"git dir: check: {p.returncode} {codes(p)}")
    p = run([compiler, "run", "--interp"], app)
    if p.returncode != 0 or p.stdout.split() != ["6", "shared"]:
        problems.append(f"git dir: run --interp: {p.returncode} {p.stdout!r} {p.stderr[-300:]!r}")
    p = run([compiler, "build"], app)
    exe = os.path.join(app, "out", "app.exe" if WINDOWS else "app")
    if p.returncode != 0 or run([exe], app).stdout.split() != ["6", "shared"]:
        problems.append(f"git dir: build: {p.returncode} {p.stderr[-400:]!r}")
    # shapes' file in its directory of the checkout, as an editor opens it there: the language server and `check`
    # check it as its package, which embeds from the checkout's assets/ (the whole checkout, not its directory).
    checkouts = os.path.join(os.environ["CEESAFE_HOME"], "git", "checkouts")
    found = [top for top, _, files in os.walk(checkouts)
             if "lib.cee" in files and top.replace("\\", "/").endswith("/shapes/src")]
    if len(found) != 1:
        problems.append(f"git dir: one checkout of shapes' directory in the package cache: {found}")
    else:
        published = language_server_diagnostics(compiler, os.path.join(found[0], "lib.cee"))
        if "publishDiagnostics" not in published or '"code"' in published:
            problems.append(f"git dir: the language server in shapes' checkout: {published[:400]}")
        p = run([compiler, "check", "--diagnostics-format=json"], os.path.dirname(found[0]))
        if p.returncode != 0 or codes(p):
            problems.append(f"git dir: check in shapes' directory of the checkout: {p.returncode} {codes(p)}")
    # Offline, without the remote: the cache has it.
    moved = remote + ".away"
    os.rename(remote, moved)
    p = run([compiler, "check", "--offline"], app)
    q = run([compiler, "fetch", "--offline"], app)
    os.rename(moved, remote)
    if p.returncode != 0 or q.returncode != 0:
        problems.append(f"git dir: --offline: {p.returncode} {p.stderr[:300]!r} {q.returncode} {q.stderr[:300]!r}")
    # Another directory than the lock's is N0324; the same one spelled otherwise is not.
    application(app, f'shapes = {{ git = "{url}", tag = "v1", dir = "shapes/../shapes" }}\n'
                     f'extra = {{ git = "{url}", tag = "v1", dir = "extra" }}\n')
    p = run([compiler, "check", "--diagnostics-format=json"], app)
    if p.returncode != 0 or codes(p):
        problems.append(f"git dir: the lock's directory spelled otherwise: {p.returncode} {codes(p)}")
    application(app, f'shapes = {{ git = "{url}", tag = "v1", dir = "shapes2" }}\n'
                     f'extra = {{ git = "{url}", tag = "v1", dir = "extra" }}\n')
    p = run([compiler, "check", "--diagnostics-format=json"], app)
    if p.returncode != 1 or codes(p) != ["N0324"]:
        problems.append(f"git dir: a directory the lock does not record: {p.returncode} {codes(p)}")

    # Spelling: N0321 when the manifest is read.
    spelling = os.path.join(base, "spelling")
    for written in ("../shapes", "/shapes", "shapes/../..", "a\\\\b", "$HOME/x", "~/x", ""):
        application(spelling, f'shapes = {{ git = "{url}", tag = "v1", dir = "{written}" }}\n', "fn main() {}\n")
        p = run([compiler, "fetch"], spelling)
        if p.returncode != 1 or fetch_codes(p) != ["N0321"]:
            problems.append(f"git dir: dir = {written!r}: expected N0321: {p.returncode} {p.stderr[:300]}")
    application(spelling, 'shapes = { path = "../x", dir = "shapes" }\n', "fn main() {}\n")
    p = run([compiler, "fetch"], spelling)
    if p.returncode != 1 or fetch_codes(p) != ["N0321"]:
        problems.append(f"git dir: a path dependency with dir: expected N0321: {p.returncode} {p.stderr[:300]}")

    # Where it leads: N0322 for a directory without a manifest, a path out of the checkout, and a symbolic link out.
    cases = [("missing", f'shapes = {{ git = "{url}", tag = "v1", dir = "assets" }}\n', "no ceesafe.toml"),
             ("outside", f'extra = {{ git = "{url}", tag = "v3", dir = "extra" }}\n',
              "is not a directory of its checkout")]
    if not WINDOWS:
        shutil.copytree(os.path.join(repository, "shapes"), os.path.join(base, "elsewhere"))
        os.symlink(os.path.join(base, "elsewhere"), os.path.join(repository, "linked"))
        commit_and_tag("v4")
        cases.append(("linked", f'shapes = {{ git = "{url}", tag = "v4", dir = "linked" }}\n', "symbolic link"))
    for name, dependencies, said in cases:
        directory = application(os.path.join(base, name), dependencies, "fn main() {}\n")
        p = run([compiler, "fetch"], directory)
        if p.returncode != 1 or fetch_codes(p) != ["N0322"] or said not in p.stderr:
            problems.append(f"git dir: {name}: expected N0322 saying {said!r}: {p.returncode} {p.stderr[:400]}")

    # Two directories under one name, at two tags: N0326 naming both.
    directory = application(os.path.join(base, "two"), f'shapes = {{ git = "{url}", tag = "v1", dir = "shapes" }}\n'
                                                       f'extra = {{ git = "{url}", tag = "v2", dir = "extra" }}\n')
    p = run([compiler, "fetch"], directory)
    if (p.returncode != 1 or fetch_codes(p) != ["N0326"] or "directory 'shapes'" not in p.stderr
            or "directory 'shapes2'" not in p.stderr):
        problems.append(f"git dir: two directories under one name: {p.returncode} {p.stderr[:400]}")

    # The registry: publish in a subdirectory writes dir; index-check and an application using it agree.
    index = os.path.join(base, "index")
    write(os.path.join(repository, "tally", "ceesafe.toml"),
          '[package]\nname = "tally"\nversion = "0.1.0"\nkind = "library"\n')
    write(os.path.join(repository, "tally", "src", "lib.cee"), "public fn three() -> i32 => 3;\n")
    commit_and_tag("v5")
    run(git + ["fetch", "-q", "origin"], repository)
    p = run([compiler, "publish"], os.path.join(repository, "tally"))
    if p.returncode != 0 or 'dir = "tally"' not in p.stdout:
        problems.append(f"git dir: publish in a subdirectory: {p.returncode} {p.stdout[:300]!r} {p.stderr[:300]!r}")
        return
    write(os.path.join(index, "packages", "tally.toml"), p.stdout)
    for argv in (["init", "-q"], ["add", "-A"], ["commit", "-qm", "one"]):
        run(git + argv, index)
    p = run([compiler, "index-check", "."], index)
    if p.returncode != 0:
        problems.append(f"git dir: index-check of an entry with dir: {p.returncode} {p.stderr[:300]!r}")
    user = application(os.path.join(base, "user"), 'tally = "0.1"\n',
                       "fn main() {\n    Console.writeLine(tally.three());\n}\n")
    p = run([compiler, "fetch"], user, {"CEESAFE_INDEX": index})
    q = run([compiler, "run", "--interp"], user, {"CEESAFE_INDEX": index})
    lock = read(os.path.join(user, "ceesafe.lock"))
    if p.returncode != 0 or q.stdout.strip() != "3" or 'dir = "tally"' not in lock:
        problems.append(f"git dir: a registry package in a subdirectory: {p.stderr[:300]!r} {q.stdout!r} "
                        f"{q.stderr[:300]!r} {lock!r}")


def dependency_patch(compiler, work, problems):
    """42. [patch] (ADR 0174): a package's source replaced everywhere in the build by the root's choice."""
    base = os.path.join(work, "dependency-patch")

    def write(path, text):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)

    def fetch_codes(p):
        return re.findall(r"error\[(N[0-9]{4})\]", p.stderr)

    for name, value in (("shapes", "2"), ("shapes-local", "20")):
        write(os.path.join(base, name, "ceesafe.toml"), '[package]\nname = "shapes"\nkind = "library"\n')
        write(os.path.join(base, name, "src", "lib.cee"), f"public fn two() -> i32 => {value};\n")
    write(os.path.join(base, "extra", "ceesafe.toml"),
          '[package]\nname = "extra"\nkind = "library"\n\n[dependencies]\nshapes = { path = "../shapes" }\n\n'
          '[patch]\n# a dependency\'s patch: not read\nshapes = { path = "../nowhere" }\n')
    write(os.path.join(base, "extra", "src", "lib.cee"), "public fn four() -> i32 => shapes.two() * 2;\n")
    app = os.path.join(base, "app")
    write(os.path.join(app, "src", "main.cee"),
          "fn main() {\n    Console.writeLine(extra.four());\n    Console.writeLine(shapes.two());\n}\n")
    dependencies = '[package]\nname = "app"\n\n[dependencies]\nextra = { path = "../extra" }\nshapes = { path = "../shapes-local" }\n'

    # Two sources for shapes: N0326.
    write(os.path.join(app, "ceesafe.toml"), dependencies)
    p = run([compiler, "fetch"], app)
    if p.returncode == 0 or fetch_codes(p) != ["N0326"]:
        problems.append(f"patch: without it, N0326: {p.returncode} {fetch_codes(p)} {p.stderr[:300]!r}")

    # The root's patch: one shapes, from it, for extra too.
    write(os.path.join(app, "ceesafe.toml"), dependencies + '\n[patch]\nshapes = { path = "../shapes-local" }\n')
    p = run([compiler, "fetch"], app)
    lock = open(os.path.join(app, "ceesafe.lock"), encoding="utf-8").read() if p.returncode == 0 else ""
    if p.returncode != 0 or lock.count('name = "shapes"') != 1 or 'source = "path+../shapes-local"' not in lock:
        problems.append(f"patch: fetch: {p.returncode} {p.stderr[:300]!r} lock {lock!r}")
    p = run([compiler, "check"], app)
    if p.returncode != 0:
        problems.append(f"patch: check: {p.returncode} {p.stderr[:300]!r}")
    p = run([compiler, "run", "--interp"], app)
    if p.returncode != 0 or p.stdout.split() != ["40", "20"]:
        problems.append(f"patch: run --interp: {p.returncode} {p.stdout!r} {p.stderr[-300:]!r}")
    p = run([compiler, "build"], app)
    exe = os.path.join(app, "out", "app.exe" if WINDOWS else "app")
    if p.returncode != 0 or run([exe], app).stdout.split() != ["40", "20"]:
        problems.append(f"patch: build: {p.returncode} {p.stderr[-300:]!r}")

    # A patch of no package of the build, and one with no source, are N0321.
    for extra_patch, label in (('nothing = { path = "../shapes" }\n', "unused"), ('shapes = "1.0"\n', "no source")):
        write(os.path.join(app, "ceesafe.toml"),
              dependencies + '\n[patch]\nshapes = { path = "../shapes-local" }\n' + extra_patch
              if label == "unused" else dependencies + "\n[patch]\n" + extra_patch)
        p = run([compiler, "fetch"], app)
        if p.returncode == 0 or "N0321" not in fetch_codes(p):
            problems.append(f"patch: {label}: N0321: {p.returncode} {fetch_codes(p)} {p.stderr[:300]!r}")


def manifest_targets(compiler, work, problems):
    """36. manifest-targets (ADR 0171): a manifest's table for the system the build is for. A package with different
    `links` under [target.<host>], [target.android] and [target.ios]: `--emit c` lists the host's without --target,
    and the named system's and no other with it; a dependency's tables follow. A name that is not a system, and a
    phone with `run`, are usage errors; [target.haiku] is still refused, in a message that names the six."""
    base = os.path.join(work, "manifest-targets")
    host = "windows" if WINDOWS else ("macos" if sys.platform == "darwin" else
                                      ("freebsd" if sys.platform.startswith("freebsd") else "linux"))
    tables = f'[target.{host}]\nlinks = ["hostthing"]\n\n[target.android]\nlinks = ["droidthing"]\n\n' \
             '[target.ios]\nlinks = ["phonething"]\n'
    for name, extra in (("dep", ""), ("app", '\n[dependencies]\ndep = { path = "../dep" }\n')):
        os.makedirs(os.path.join(base, name, "src"))
        kind = "executable" if name == "app" else "library"
        with open(os.path.join(base, name, "ceesafe.toml"), "w") as f:
            f.write(f'[package]\nname = "{name}"\nversion = "0.1.0"\nkind = "{kind}"\n\n'
                    + tables.replace("thing", "thing" + name) + extra)
    with open(os.path.join(base, "dep", "src", "dep.cee"), "w") as f:
        f.write("public fn one() -> i32 => 1;\n")
    with open(os.path.join(base, "app", "src", "main.cee"), "w") as f:
        f.write("import dep;\nfn main() -> i32 => one() - 1;\n")
    app = os.path.join(base, "app")
    p = run([compiler, "fetch"], app)
    if p.returncode != 0:
        problems.append(f"manifest-targets: fetch failed: {p.stderr.strip()[-300:]}")
        return
    for target, want in ((None, "host"), ("android", "droid"), ("ios", "phone")):
        out = "c-" + (target or "host")
        p = run([compiler, "build", "--emit", "c", "-o", out] + (["--target", target] if target else []), app)
        link = os.path.join(app, out, "ceesafe_link.txt")
        text = open(link).read() if os.path.exists(link) else ""
        names = sorted(w for w in ("host", "droid", "phone") if w + "thing" in text)
        both = (want + "thingapp") in text and (want + "thingdep") in text
        if p.returncode != 0 or names != [want] or not both:
            problems.append(f"manifest-targets: --target {target}: the link names {names}, not the package's and "
                            f"its dependency's {want} libraries only: {p.stderr.strip()[-300:]}")
    p = run([compiler, "check", "--target", "android"], app)
    if p.returncode != 0:
        problems.append(f"manifest-targets: check --target android failed: {p.stderr.strip()[-300:]}")
    p = run([compiler, "build", "--target", "plan9"], app)
    if p.returncode != 2 or "'android'" not in p.stderr:
        problems.append(f"manifest-targets: --target plan9 was not a usage error naming the systems: {p.returncode}")
    p = run([compiler, "run", "--target", "android"], app)
    if p.returncode != 2 or "cannot run" not in p.stderr:
        problems.append(f"manifest-targets: run --target android was not a usage error: {p.returncode}")
    with open(os.path.join(app, "ceesafe.toml"), "a") as f:
        f.write('\n[target.haiku]\nlinks = ["x"]\n')
    p = run([compiler, "check"], app)
    if p.returncode == 0 or "[target.android]" not in p.stderr:
        problems.append("manifest-targets: [target.haiku] was not refused in a message that names [target.android]")


def manifest_frameworks(compiler, work, problems):
    """43. frameworks (ADR 0184): Apple frameworks named in [target.macos] and [target.ios]. `--emit c` for macOS and
    iOS lists each as `-framework NAME` in the link file, once though two packages name it; for Linux, none. In
    [package] or a non-Apple target table the field is refused. On a Mac, a program that calls IOKit and
    CoreFoundation links with no C file and no flag, and runs."""
    base = os.path.join(work, "manifest-frameworks")
    for name, extra in (("dep", ""), ("app", '\n[dependencies]\ndep = { path = "../dep" }\n')):
        os.makedirs(os.path.join(base, name, "src"))
        kind = "executable" if name == "app" else "library"
        with open(os.path.join(base, name, "ceesafe.toml"), "w") as f:
            f.write(f'[package]\nname = "{name}"\nversion = "0.1.0"\nkind = "{kind}"\n\n'
                    '[target.macos]\nframeworks = ["IOKit", "CoreFoundation"]\n\n'
                    '[target.ios]\nframeworks = ["UIKit"]\n' + extra)
    with open(os.path.join(base, "dep", "src", "dep.cee"), "w") as f:
        f.write("public fn one() -> i32 => 1;\n")
    with open(os.path.join(base, "app", "src", "main.cee"), "w") as f:
        f.write("import dep;\nfn main() -> i32 => one() - 1;\n")
    app = os.path.join(base, "app")
    p = run([compiler, "fetch"], app)
    if p.returncode != 0:
        problems.append(f"frameworks: fetch failed: {p.stderr.strip()[-300:]}")
        return
    for target, want in (("macos", ["CoreFoundation", "IOKit"]), ("ios", ["UIKit"]), ("linux", [])):
        out = "c-" + target
        p = run([compiler, "build", "--emit", "c", "-o", out, "--target", target], app)
        link = os.path.join(app, out, "ceesafe_link.txt")
        lines = open(link).read().split("\n") if os.path.exists(link) else []
        named = sorted(lines[i + 1] for i in range(len(lines) - 1) if lines[i] == "-framework")
        if p.returncode != 0 or named != want:
            problems.append(f"frameworks: --target {target}: the link names frameworks {named}, not {want}: "
                            f"{p.stderr.strip()[-300:]}")
    for table in ("[package]", "[target.linux]"):
        bad = os.path.join(base, "bad-" + table.strip("[]").replace(".", "-"))
        os.makedirs(os.path.join(bad, "src"))
        with open(os.path.join(bad, "ceesafe.toml"), "w") as f:
            if table == "[package]":
                f.write('[package]\nname = "bad"\nframeworks = ["IOKit"]\n')
            else:
                f.write('[package]\nname = "bad"\n\n[target.linux]\nframeworks = ["IOKit"]\n')
        with open(os.path.join(bad, "src", "main.cee"), "w") as f:
            f.write("fn main() {}\n")
        p = run([compiler, "check"], bad)
        if p.returncode == 0 or "[target.macos]" not in p.stderr or "N0321" not in p.stderr:
            problems.append(f"frameworks: in {table} the field was not refused (N0321) naming [target.macos]: "
                            f"{p.returncode} {p.stderr.strip()[-200:]}")
    if sys.platform == "darwin":
        real = os.path.join(base, "iokit")
        os.makedirs(os.path.join(real, "src"))
        with open(os.path.join(real, "ceesafe.toml"), "w") as f:
            f.write('[package]\nname = "iokit"\n\n[target.macos]\nframeworks = ["IOKit", "CoreFoundation"]\n')
        with open(os.path.join(real, "src", "main.cee"), "w") as f:
            f.write('extern "C" {\n'
                    '    fn IOServiceMatching(name: *CChar) -> *void;\n'
                    '    fn CFRelease(cf: *void);\n'
                    '}\n'
                    '/// Safety: IOKit returns a dictionary we own; CFRelease frees it.\n'
                    'fn main() -> i32 {\n'
                    '    let d = unsafe { IOServiceMatching(c"IOService".ptr()) };\n'
                    '    if (d.isNull()) {\n'
                    '        return 1;\n'
                    '    }\n'
                    '    unsafe { CFRelease(d); }\n'
                    '    Console.writeLine("linked");\n'
                    '    return 0;\n'
                    '}\n')
        p = run([compiler, "build"], real)
        exe = os.path.join(real, "out", "iokit")
        q = run([exe], real) if p.returncode == 0 else None
        if p.returncode != 0 or q is None or q.returncode != 0 or q.stdout.strip() != "linked":
            problems.append(f"frameworks: a program calling IOKit did not build and run on macOS: "
                            f"{p.stderr.strip()[-300:]}")


def library_build_default(compiler, work, problems):
    """44. library-build-default (ADR 0187): a library package's `build` makes its static library."""
    pkg = os.path.join(work, "library-build-default")
    os.makedirs(os.path.join(pkg, "src"))
    with open(os.path.join(pkg, "ceesafe.toml"), "w") as f:
        f.write('[package]\nname = "shelf"\nversion = "0.1.0"\nkind = "library"\n')
    with open(os.path.join(pkg, "src", "shelf.cee"), "w") as f:
        f.write("public fn width() -> i32 => 3;\n")
    p = run([compiler, "build"], pkg)
    archive = os.path.join(pkg, "out", "shelf.a")
    header = os.path.join(pkg, "out", "ceesafe", "embed.h")
    if p.returncode != 0 or "N0300" in p.stderr or not os.path.isfile(archive) or not os.path.isfile(header):
        problems.append(f"library-build-default: build did not write out/shelf.a and out/ceesafe/embed.h "
                        f"(status {p.returncode}): {p.stderr.strip()[-300:]}")
    p = run([compiler, "build", "--emit", "exe"], pkg)
    if p.returncode == 0 or "N0300" not in p.stderr:
        problems.append(f"library-build-default: --emit exe in a library was not N0300 (status {p.returncode}): "
                        f"{p.stderr.strip()[-300:]}")


def pe_subsystem(path):
    """A PE executable's subsystem: 2 GUI, 3 console; None if the file is not one."""
    try:
        with open(path, "rb") as f:
            data = f.read(4096)
    except OSError:
        return None
    if data[:2] != b"MZ":
        return None
    at = int.from_bytes(data[0x3C:0x40], "little")
    if data[at:at + 4] != b"PE\0\0":
        return None
    return int.from_bytes(data[at + 24 + 68:at + 24 + 70], "little")


def windowed_executables(compiler, work, problems):
    """39. A windowed executable (ADR 0151): the field, the option, the start in `main`, the subsystem on Windows."""
    base = os.path.join(work, "windowed")
    app = os.path.join(base, "app")

    def write(path, text):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)

    def manifest(fields, extra=""):
        write(os.path.join(app, "ceesafe.toml"), '[package]\nname = "app"\n' + fields + extra)

    def codes(p):
        try:
            return [(d["code"], d["start"]["line"]) for d in json.loads(p.stdout)["diagnostics"]]
        except (ValueError, KeyError):
            return [p.stdout[:200] + p.stderr[:200]]

    write(os.path.join(app, "src", "main.cee"), 'fn main() {\n    Console.writeLine("windowed");\n}\n')
    write(os.path.join(app, "tests", "one.cee"), "@test\nfn one() {\n    assert(1 + 1 == 2);\n}\n")
    exe = os.path.join(app, "out", "app.exe" if WINDOWS else "app")
    manifest("windowed = true\n")
    p = run([compiler, "check"], app)
    if p.returncode != 0:
        problems.append(f"windowed: check failed: {p.stderr[:300]}")
    for flags in ([], ["--split"]):
        p = run([compiler, "build"] + flags, app)
        if p.returncode != 0:
            problems.append(f"windowed: build {flags} failed: {p.stderr[-400:]}")
            continue
        q = run([exe], app)
        if q.returncode != 0 or q.stdout.strip() != "windowed":
            problems.append(f"windowed: the program {flags} printed {q.stdout!r} ({q.returncode}) {q.stderr[:200]}")
        if WINDOWS and pe_subsystem(exe) != 2:
            problems.append(f"windowed: build {flags}: the PE subsystem is {pe_subsystem(exe)}, not 2 (GUI)")
    for name, fields, flags, expected in (("field", "windowed = true\n", [], 1), ("none", "", [], 0),
                                          ("false", "windowed = false\n", [], 0), ("option", "", ["--windowed"], 1)):
        manifest(fields)
        directory = os.path.join(base, "c-" + name)
        p = run([compiler, "build", "--emit", "c", "-o", directory] + flags, app)
        try:
            with open(os.path.join(directory, "main.c"), encoding="utf-8") as f:
                text = f.read()
        except OSError:
            text = ""
        main = text[text.find("int main(int argc, char **argv) {"):].split("\n")
        found = sum(1 for line in main if line.strip() == "ceesafe_rt_windowed();")
        first = len(main) > 1 and main[1].strip() == "ceesafe_rt_windowed();"
        if p.returncode != 0 or found != expected or (expected and not first):
            problems.append(f"windowed: --emit c ({name}): main calls ceesafe_rt_windowed() {found} times, not "
                            f"{expected} (first): {p.returncode} {p.stderr[:200]}")
    if WINDOWS:
        manifest("")
        p = run([compiler, "build"], app)
        if p.returncode != 0 or pe_subsystem(exe) != 3:
            problems.append(f"windowed: without the field the PE subsystem is {pe_subsystem(exe)}, not 3 (console)")
    manifest("windowed = true\n")
    p = run([compiler, "test"], app)
    if p.returncode != 0:
        problems.append(f"windowed: test failed: {p.stdout[-300:]} {p.stderr[-300:]}")
    elif WINDOWS and pe_subsystem(os.path.join(app, "out", "app-tests.exe")) != 3:
        problems.append("windowed: the test program of a windowed package is not a console program")
    for name, fields, extra in (("library", 'kind = "library"\nwindowed = true\n', ""),
                                ("object", 'kind = "object"\nwindowed = true\n', ""),
                                ("freestanding", 'profile = "freestanding"\nwindowed = true\n', ""),
                                ("not a boolean", 'windowed = "yes"\n', ""),
                                ("target table", "", "\n[target.windows]\nwindowed = true\n")):
        manifest(fields, extra)
        p = run([compiler, "check", "--diagnostics-format=json"], app)
        line = 4 if not extra else 5
        if name == "not a boolean":
            line = 3
        if p.returncode != 1 or codes(p) != [("N0321", line)]:
            problems.append(f"windowed: {name}: expected one N0321 on line {line}: {codes(p)}")
    manifest("")
    for flags in (["--emit", "obj"], ["--emit", "lib"], ["--profile", "freestanding"]):
        p = run([compiler, "build", "--windowed"] + flags, app)
        if p.returncode != 2 or "--windowed is for an executable of the hosted profile" not in p.stderr:
            problems.append(f"windowed: --windowed {' '.join(flags)} is not a usage error: {p.returncode} "
                            f"{p.stderr[:200]}")
    manifest('profile = "freestanding"\n')
    p = run([compiler, "build", "--windowed"], app)
    if p.returncode != 2 or "--windowed is for an executable of the hosted profile" not in p.stderr:
        problems.append(f"windowed: --windowed in a freestanding package is not a usage error: {p.returncode}")


POLLS_PROGRAM = '''enum Fail {
    Crashed,
    NoMemory,
}

// Computes for up to 3 s without waiting: the bound ends it long before, where there are polls.
fn spin() -> u64 {
    let until = Clock.monotonicNanos() + 3000000000;
    var n: u64 = 0;
    while (Clock.monotonicNanos() < until) {
        n += 1;
    }
    return n;
}

async fn spinner() -> Result<void, Fail> {
    let _ = spin();
    return Ok();
}

async fn work() -> Result<void, Fail> {
    return await Async.tasks(4, Fail.NoMemory, async (set) => {
        await set.spawnIsolated(Fail.Crashed, async move () => await spinner())?;
        return Ok();
    });
}

fn main() {
    if (!Async.runBound(50, RunBound.Count)) {
        Console.writeLine("no polls");
        return;
    }
    match (Async.run(work())) {
        Err(Fail.Crashed) => { Console.write("ended, overruns "); }
        _ => { Console.write("not ended, overruns "); }
    }
    Console.writeLine(Async.overruns());
}
'''

POLLS_LINE = "#define CEE_POLLS 1 /* polls: the root manifest's polls = true, or --polls (ADR 0190) */"


def polls_from_manifest(compiler, work, problems):
    """45. Polls asked for by the root manifest (ADR 0190): the key, the options, a dependency's key, `test`, the C."""
    base = os.path.join(work, "polls")
    app = os.path.join(base, "app")
    dep = os.path.join(base, "dep")

    def write(path, text):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)

    def manifest(fields, extra=""):
        write(os.path.join(app, "ceesafe.toml"), '[package]\nname = "app"\n' + fields + extra)

    def codes(p):
        try:
            return [(d["code"], d["start"]["line"]) for d in json.loads(p.stdout)["diagnostics"]]
        except (ValueError, KeyError):
            return [p.stdout[:200] + p.stderr[:200]]

    exe = os.path.join(app, "out", "app.exe" if WINDOWS else "app")
    write(os.path.join(app, "src", "main.cee"), POLLS_PROGRAM)
    write(os.path.join(app, "tests", "bound.cee"),
          "@test\nfn bounded() {\n    assert(Async.runBound(50, RunBound.Count));\n}\n")
    ended, none = "ended, overruns 1", "no polls"
    for name, fields, flags, expected in (("key", "polls = true\n", [], ended), ("no key", "", [], none),
                                          ("false", "polls = false\n", [], none),
                                          ("--no-polls", "polls = true\n", ["--no-polls"], none),
                                          ("--polls", "", ["--polls"], ended),
                                          ("--split", "polls = true\n", ["--split"], ended)):
        manifest(fields)
        p = run([compiler, "build"] + flags, app)
        if p.returncode != 0:
            problems.append(f"polls: build ({name}) failed: {p.stderr[-400:]}")
            continue
        q = run([exe], app)
        if q.stdout.strip() != expected:
            problems.append(f"polls: the program ({name}) printed {q.stdout.strip()!r}, not {expected!r}")
    # A dependency's key changes nothing for a root without it.
    write(os.path.join(dep, "ceesafe.toml"), '[package]\nname = "dep"\nkind = "library"\npolls = true\n')
    write(os.path.join(dep, "src", "dep.cee"), "public fn one() -> i32 => 1;\n")
    manifest("", '\n[dependencies]\ndep = { path = "../dep" }\n')
    p = run([compiler, "fetch"], app)
    p = run([compiler, "build"], app) if p.returncode == 0 else p
    q = run([exe], app) if p.returncode == 0 else p
    if p.returncode != 0 or q.stdout.strip() != none:
        problems.append(f"polls: a dependency's key reached the root: {q.stdout.strip()!r} {p.stderr[-300:]}")
    lock = os.path.join(app, "ceesafe.lock")
    if os.path.exists(lock):
        os.remove(lock)
    # `test` builds its harness with the root's polls.
    manifest("polls = true\n")
    p = run([compiler, "test"], app)
    if p.returncode != 0:
        problems.append(f"polls: test with the key failed: {p.stdout[-300:]} {p.stderr[-300:]}")
    p = run([compiler, "test", "--no-polls"], app)
    if p.returncode == 0:
        problems.append("polls: test --no-polls passed a test that needs polls")
    # --emit c: each C file the build writes defines the macro, and builds the same program with no flag.
    directory = os.path.join(base, "c")
    p = run([compiler, "build", "--emit", "c", "-o", directory], app)
    sources = sorted(f for f in os.listdir(directory) if f.endswith(".c")) if p.returncode == 0 else []
    if not sources:
        problems.append(f"polls: --emit c wrote no C: {p.stderr[-300:]}")
    for source in sources:
        with open(os.path.join(directory, source), encoding="utf-8") as f:
            if f.readline().rstrip("\n") != POLLS_LINE:
                problems.append(f"polls: --emit c: {source} does not start with the polls line")
    if sources and not WINDOWS:
        program = os.path.join(base, "from-c")
        p = run(["cc", "-std=c11", "-O2", "-I", directory] + [os.path.join(directory, s) for s in sources] +
                ["-o", program, "-lpthread", "-lm"], base)
        q = run([program], base) if p.returncode == 0 else p
        if p.returncode != 0 or q.stdout.strip() != ended:
            problems.append(f"polls: --emit c compiled with no flag printed {q.stdout.strip()!r}: {p.stderr[-300:]}")
    manifest("")
    none_c = os.path.join(base, "c-none")
    p = run([compiler, "build", "--emit", "c", "-o", none_c], app)
    first = ""
    if p.returncode == 0:
        with open(os.path.join(none_c, "main.c"), encoding="utf-8") as f:
            first = f.readline()
    if p.returncode != 0 or "CEE_POLLS" in first:
        problems.append(f"polls: --emit c without the key: {p.returncode} {first.strip()!r}")
    # The key's errors: one N0321 each.
    for name, fields, extra, line in (("not a boolean", 'polls = "yes"\n', "", 3),
                                      ("freestanding", 'profile = "freestanding"\npolls = true\n', "", 4),
                                      ("target table", "", "\n[target.linux]\npolls = true\n", 5)):
        manifest(fields, extra)
        p = run([compiler, "check", "--diagnostics-format=json"], app)
        if p.returncode != 1 or codes(p) != [("N0321", line)]:
            problems.append(f"polls: {name}: expected one N0321 on line {line}: {codes(p)}")
    # The options' usage errors (status 2).
    manifest("")
    for flags, text in ((["--polls", "--no-polls"], "--polls and --no-polls contradict"),
                        (["--no-polls", "--cflag", "-DCEE_POLLS=1"], "--no-polls and --cflag -DCEE_POLLS contradict"),
                        (["--polls", "--profile", "freestanding"], "--polls is for the hosted profile")):
        p = run([compiler, "build"] + flags, app)
        if p.returncode != 2 or text not in p.stderr:
            problems.append(f"polls: build {' '.join(flags)} is not a usage error: {p.returncode} {p.stderr[:200]}")


def file_private_names(compiler, work, problems):
    """46. Each file's private items have a scope of their own (ADR 0230)."""
    base = os.path.join(work, "file-private-names")
    shutil.copytree(os.path.join(ROOT, "tests", "packages", "file-private-names"), base)
    p = run([compiler, "check"], base)
    if p.returncode != 0:
        problems.append(f"file-private-names: check failed: {p.stdout[-600:]}{p.stderr[-600:]}")
    p = run([compiler, "run", "--interp"], base)
    if p.returncode != 0 or p.stdout.split() != ["1148", "74"]:
        problems.append(f"file-private-names: the interpreter should print 1148 and 74: {p.returncode} "
                        f"{p.stdout!r} {p.stderr[-300:]}")
    p = run([compiler, "build"], base)
    exe = os.path.join(base, "out", "fileprivatenames.exe" if WINDOWS else "fileprivatenames")
    q = run([exe], base) if p.returncode == 0 else p
    if p.returncode != 0 or q.stdout.split() != ["1148", "74"]:
        problems.append(f"file-private-names: the built program should print 1148 and 74: {p.returncode} "
                        f"{q.stdout!r} {p.stderr[-600:]}")
    emitted = os.path.join(work, "file-private-names-c")
    p = run([compiler, "build", "--emit", "c", "-o", emitted], base)
    text = ""
    if p.returncode == 0:
        for name in os.listdir(emitted):
            if name.endswith(".c"):
                with open(os.path.join(emitted, name), encoding="utf-8") as f:
                    text += f.read()
    marked = sorted(set(re.findall(r"ceeF_00[0-9]+_appendInteger", text)))
    if p.returncode != 0 or len(marked) != 2:
        problems.append(f"file-private-names: the C should define two marked appendInteger: {marked} "
                        f"{p.stderr[-300:]}")
    other = os.path.join(base, "src", "other.cee")
    with open(other, "w", encoding="utf-8") as f:
        f.write("fn other() -> i64 {\n    var out = Vector<u8>.create();\n    appendInteger(mut out, 1);\n"
                "    let r: Rwo = otherRow();\n    return 0;\n}\n")
    p = run([compiler, "check", "--diagnostics-format=json"], base)
    try:
        diagnostics = json.loads(p.stdout)["diagnostics"]
        found = sorted((d["code"], os.path.basename(d["file"]), d["start"]["line"]) for d in diagnostics)
        messages = " ".join(d["message"] for d in diagnostics)
    except (ValueError, KeyError):
        found, messages = [p.stdout[:300] + p.stderr[:300]], ""
    if (p.returncode != 1 or ("N0296", "other.cee", 3) not in found or ("N0204", "other.cee", 4) not in found
            or "is private to src/" not in messages or "'Row'" in messages):
        problems.append(f"file-private-names: expected N0296 at other.cee:3 naming the file, and N0204 at 4 that "
                        f"offers no private 'Row': {p.returncode} {found} {messages[:400]}")
    os.remove(other)
    source = os.path.join(base, "src", "datetime.cee")
    with open(source, encoding="utf-8") as f:
        lines = f.read().split("\n")
    call = next(k for k, line in enumerate(lines) if line.strip().startswith("appendInteger(mut out"))
    _, answered = language_server(compiler, source, (call, lines[call].index("appendInteger") + 2),
                                  "textDocument/definition")
    if "datetime.cee" not in answered or '"line": 1,' not in answered:
        problems.append(f"file-private-names: the definition of appendInteger in datetime.cee should be its own, "
                        f"line 2: {answered[:300]!r}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    problems = []
    with tempfile.TemporaryDirectory(prefix="ceesafe-packages-") as work:
        os.environ["CEESAFE_HOME"] = os.path.join(work, "home")  # git checkouts here, not in the user's ~/.ceesafe
        for name in ("native-links", "native-errors"):
            shutil.copytree(os.path.join(ROOT, "tests", "packages", name), os.path.join(work, name))
        lib = build_library(work)
        env = {"CEESAFE_TEST_LIBDIR": lib}
        app = os.path.join(work, "native-links", "app")

        # 1. Two levels of dependency above the library.
        p = run([compiler, "fetch"], app, env)
        if p.returncode != 0:
            problems.append(f"fetch failed: {p.stderr.strip()}")
        p = run([compiler, "build"], app, env)
        exe = os.path.join(app, "out", "app.exe" if WINDOWS else "app")
        if p.returncode != 0:
            problems.append(f"build failed: {p.stderr.strip()[-600:]}")
        else:
            out = run([exe], app).stdout.strip()
            if out != "42":
                problems.append(f"the program printed {out!r}, not 42")

        # 2. The link file of --emit c.
        cdir = os.path.join(work, "app-c")
        p = run([compiler, "build", "--emit", "c", "-o", cdir], app, env)
        link = os.path.join(cdir, "ceesafe_link.txt")
        if p.returncode != 0 or not os.path.exists(link):
            problems.append(f"--emit c wrote no ceesafe_link.txt: {p.stderr.strip()[-300:]}")
        else:
            lines = open(link).read().splitlines()
            msvc = "cl" in os.path.basename(os.environ.get("CC", "")).lower() and WINDOWS
            def same(a, b):
                return os.path.normcase(os.path.realpath(a)) == os.path.normcase(os.path.realpath(b))
            def flagged(prefix, path):
                return any(l.startswith(prefix) and same(l[len(prefix):], path) for l in lines)
            if msvc:
                ok = flagged("/LIBPATH:", lib) and "mylib.lib" in lines
            else:
                ok = flagged("-L", lib) and "-lmylib" in lines
            ok = ok and any(same(l, os.path.join(work, "native-links", "wrap", "c", "shim.c")) for l in lines
                            if not l.startswith(("-", "/I", "/L")))
            if not ok:
                problems.append(f"ceesafe_link.txt lacks the library, its directory or wrap's C file: {lines}")
            if not any(l.startswith(("-I", "/I")) and l.rstrip("/\\").endswith("include") for l in lines):
                problems.append(f"ceesafe_link.txt lacks wrap's include directory: {lines}")

        # 3. An unset variable.
        p = run([compiler, "build"], app)
        if p.returncode == 0 or "N0321" not in p.stderr or "CEESAFE_TEST_LIBDIR" not in p.stderr:
            problems.append(f"an unset variable was not N0321 naming it: {p.stderr.strip()[:300]}")

        # 4. The package hash (tooling.md §4.3: a git or registry dependency's, which `publish` prints) follows
        #    `links`: wrap in a git repository of its own, published before and after the change.
        wrap = os.path.join(work, "native-links", "wrap")
        git = ["git", "-c", "user.name=test", "-c", "user.email=test@localhost"]
        run(git + ["init", "-q"], wrap)
        run(git + ["add", "-A"], wrap)
        run(git + ["commit", "-qm", "one"], wrap)
        user = os.path.join(work, "hash-user")
        os.makedirs(os.path.join(user, "src"))
        open(os.path.join(user, "src", "main.cee"), "w").write("fn main() {}\n")

        def locked_hash():
            commit = run(["git", "rev-parse", "HEAD"], wrap).stdout.strip()
            source = wrap.replace("\\", "/")
            open(os.path.join(user, "ceesafe.toml"), "w").write(
                f'[package]\nname = "user"\nversion = "0.1.0"\n\n[dependencies]\n'
                f'wrap = {{ git = "{source}", rev = "{commit}" }}\n')
            p = run([compiler, "fetch"], user, env)
            lock = os.path.join(user, "ceesafe.lock")
            text = open(lock).read() if os.path.exists(lock) else p.stderr
            return [l for l in text.splitlines() if l.startswith("hash")]

        before = locked_hash()
        toml = os.path.join(wrap, "ceesafe.toml")
        text = open(toml).read()
        open(toml, "w").write(text.replace('links = ["mylib"]', 'links = ["mylib", "m"]'))
        run(git + ["commit", "-qam", "two"], wrap)
        after = locked_hash()
        if not before or before == after:
            problems.append(f"the locked hash did not follow links: {before} {after}")

        # 5. Invalid values.
        p = run([compiler, "check", "--diagnostics-format=json"], os.path.join(work, "native-errors"))
        try:
            found = [d["code"] for d in json.loads(p.stdout)["diagnostics"]]
        except (ValueError, KeyError):
            found = [line for line in p.stdout.splitlines() + p.stderr.splitlines() if "N0321" in line]
        count = sum(1 for c in found if "N0321" in str(c))
        if count != 5:
            problems.append(f"native-errors: {count} N0321 diagnostics, not 5: {p.stdout[:400]}{p.stderr[:400]}")

        # 6. A method merged from an `impl` block in another file keeps that file (ADR 0064 §1): its bounds check
        #    reports the method's own position, not an offset into the struct's file.
        impl_files = os.path.join(work, "impl-files")
        shutil.copytree(os.path.join(ROOT, "tests", "packages", "impl-files"), impl_files)
        p = run([compiler, "build"], impl_files)
        if p.returncode != 0:
            problems.append(f"impl-files: build failed: {p.stdout[:400]}{p.stderr[:400]}")
        else:
            exe = os.path.join(impl_files, "out", "impl_files" + (".exe" if WINDOWS else ""))
            p = run([exe], impl_files)
            if p.returncode == 0 or p.stdout.split() != ["20"] or "src/more/cells.cee:24:" not in p.stderr.replace("\\", "/"):
                problems.append(f"impl-files: expected 20 then a bounds panic at src/more/cells.cee:24: "
                                f"{p.returncode} {p.stdout[:200]!r} {p.stderr[:200]!r}")

        # 7. A diagnostic in such a method names its own file and line.
        cells = os.path.join(impl_files, "src", "more", "cells.cee")
        with open(cells) as f:
            text = f.read()
        with open(cells, "w") as f:
            f.write(text.replace("let n = this.base;", "let n: bool = this.base;"))
        p = run([compiler, "check", "--diagnostics-format=json"], impl_files)
        try:
            diagnostics = json.loads(p.stdout)["diagnostics"]
            places = [(d["file"].replace("\\", "/"), d["start"]["line"]) for d in diagnostics]
        except (ValueError, KeyError):
            places = []
        if not places or places[0] != ("src/more/cells.cee", 23) or any(f != "src/more/cells.cee" for f, _ in places):
            problems.append(f"impl-files: expected diagnostics in src/more/cells.cee, the first at line 23: {places} "
                            f"{p.stdout[:300]}{p.stderr[:300]}")

        # 8. A dependency's `@test` in its src/ is private to it: the dependent's harness lists only its own tests.
        dependency_tests = os.path.join(work, "dependency-tests")
        shutil.copytree(os.path.join(ROOT, "tests", "packages", "dependency-tests"), dependency_tests)
        app = os.path.join(dependency_tests, "app")
        p = run([compiler, "test", "--list"], app)
        q = run([compiler, "test"], app)
        if p.returncode != 0 or p.stdout.split() != ["quadrupleOfThree"] or q.returncode != 0:
            problems.append(f"dependency-tests: expected app's one test to be listed and pass: {p.returncode} "
                            f"{p.stdout[:200]!r} {q.returncode} {q.stdout[-300:]!r}{q.stderr[-300:]!r}")

        # 9. A library that only borrows a listener: its test build has the listener's drop and the vector helpers'
        #    result type.
        listener = os.path.join(work, "borrowed-listener")
        shutil.copytree(os.path.join(ROOT, "tests", "packages", "borrowed-listener"), listener)
        p = run([compiler, "test"], listener)
        if p.returncode != 0:
            problems.append(f"borrowed-listener: test failed: {p.stdout[-400:]}{p.stderr[-400:]}")

        # 10. Warnings are the root package's only, and never fail a command unless --deny-warnings is given.
        warnings = os.path.join(work, "dependency-warnings")
        shutil.copytree(os.path.join(ROOT, "tests", "packages", "dependency-warnings"), warnings)
        app = os.path.join(warnings, "app")
        p = run([compiler, "check", "--diagnostics-format=json"], app)
        q = run([compiler, "check", "--deny-warnings"], app)
        try:
            document = json.loads(p.stdout)
            found = [(d["code"], d["severity"], d["file"].replace("\\", "/")) for d in document["diagnostics"]]
        except (ValueError, KeyError):
            found = []
        if p.returncode != 0 or found != [("N0400", "warning", "src/main.cee")] or q.returncode != 1:
            problems.append(f"dependency-warnings: expected app's one warning, status 0, and 1 with --deny-warnings: "
                            f"{p.returncode} {found} {q.returncode} {p.stdout[:300]}{p.stderr[:300]}")

        # 11. A local may shadow a dependency (ADR 0074 §2.10); associated constants through a package (§2.7).
        shadowing = os.path.join(work, "shadowing")
        shutil.copytree(os.path.join(ROOT, "tests", "packages", "shadowing"), shadowing)
        app = os.path.join(shadowing, "app")
        expected = "466\n993\n2\n7\ntrue\n465\n0\n1\n5\n587"
        p = run([compiler, "check", "--diagnostics-format=json"], app)
        try:
            found = [(d["code"], d["severity"], d["start"]["line"]) for d in json.loads(p.stdout)["diagnostics"]]
        except (ValueError, KeyError):
            found = []
        if p.returncode != 0 or found != [("N0401", "warning", 9), ("N0401", "warning", 33)]:
            problems.append(f"shadowing: expected two N0401 warnings (lines 9 and 33) and status 0: "
                            f"{p.returncode} {found} {p.stdout[:300]}{p.stderr[:300]}")
        p = run([compiler, "run", "--interp"], app)
        if p.returncode != 0 or p.stdout.strip() != expected:
            problems.append(f"shadowing: run --interp printed {p.stdout.strip()!r} ({p.returncode}) {p.stderr[-300:]}")
        p = run([compiler, "build"], app)
        exe = os.path.join(app, "out", "app.exe" if WINDOWS else "app")
        if p.returncode != 0:
            problems.append(f"shadowing: build failed: {p.stderr.strip()[-600:]}")
        else:
            out = run([exe], app).stdout.strip()
            if out != expected:
                problems.append(f"shadowing: the program printed {out!r}")

        # 12. N0315 for an item; N0312 for a constant that is not public.
        for name, code in (("item", "N0315"), ("errors", "N0312")):
            p = run([compiler, "check", "--diagnostics-format=json"], os.path.join(shadowing, name))
            try:
                found = [d["code"] for d in json.loads(p.stdout)["diagnostics"]]
            except (ValueError, KeyError):
                found = []
            if p.returncode != 1 or found != [code]:
                problems.append(f"shadowing/{name}: expected {code} alone: {p.returncode} {found} {p.stderr[:300]}")

        # 13. A compiler requirement (ADR 0076): one this ceesafec does not meet is N0321 naming both versions; one it
        #     meets checks.
        pinned = os.path.join(work, "compiler-requirement")
        os.makedirs(os.path.join(pinned, "src"))
        with open(os.path.join(pinned, "src", "main.cee"), "w") as f:
            f.write("fn main() {}\n")
        for requirement, accepted in ((">=999.0.0", False), (">=0.0.1, <999.0.0", True)):
            with open(os.path.join(pinned, "ceesafe.toml"), "w") as f:
                f.write(f'[package]\nname = "pinned"\nceesafec = "{requirement}"\n')
            p = run([compiler, "check"], pinned)
            needs = f"this package needs ceesafec {requirement}; this is ceesafec "
            if accepted and p.returncode != 0:
                problems.append(f"compiler-requirement: {requirement!r} was refused: {p.stderr[:300]}")
            if not accepted and (p.returncode == 0 or "N0321" not in p.stderr or needs not in p.stderr):
                problems.append(f"compiler-requirement: {requirement!r} was not N0321 naming it: {p.stderr[:300]}")

        # 14. A dependency's generic items called with explicit type arguments, `dep.f<T>(…)` and `dep.S<T>.m(…)`: the
        #     arguments go with the joined name (they were dropped until 2026-11-09, so `T` was inferred as i32).
        qualified = os.path.join(work, "qualified-generics")
        shutil.copytree(os.path.join(ROOT, "tests", "packages", "qualified-generics"), qualified)
        app = os.path.join(qualified, "app")
        expected = "29\ntrue"
        p = run([compiler, "check"], app)
        if p.returncode != 0:
            problems.append(f"qualified-generics: check failed: {p.stderr[:600]}")
        p = run([compiler, "run", "--interp"], app)
        if p.returncode != 0 or p.stdout.strip() != expected:
            problems.append(f"qualified-generics: run --interp printed {p.stdout.strip()!r} ({p.returncode}) "
                            f"{p.stderr[-300:]}")
        p = run([compiler, "build"], app)
        exe = os.path.join(app, "out", "app.exe" if WINDOWS else "app")
        if p.returncode != 0:
            problems.append(f"qualified-generics: build failed: {p.stderr.strip()[-600:]}")
        else:
            out = run([exe], app).stdout.strip()
            if out != expected:
                problems.append(f"qualified-generics: the program printed {out!r}")

        # 15. One owner per C name (ADR 0081): N0201 at the declaration that clashes, its note and hint, its fix.
        #     A note in a dependency names its file as the path from the package (ADR 0082 §2.1): ../sys/src/lib.cee.
        c_names = os.path.join(work, "c-names")
        shutil.copytree(os.path.join(ROOT, "tests", "packages", "c-names"), c_names)

        def n0201s(directory, command="check"):
            # The whole program's check (ADR 0013 D4): `check` alone would report a dependency that fails on its own
            # as one N0364 (ADR 0082 §2.7, §23–§25 below).
            extra = ["--whole-program"] if command == "check" else []
            p = run([compiler, command, "--diagnostics-format=json", *extra], directory)
            try:
                return p.returncode, [d for d in json.loads(p.stdout)["diagnostics"] if d["code"] == "N0201"]
            except (ValueError, KeyError):
                return p.returncode, [{"unparsed": p.stdout[:300] + p.stderr[:300]}]

        def where(d):
            return (d.get("file", "").replace("\\", "/"), d.get("start", {}).get("line"))

        hint = "use `sys.strlen`, which package 'sys' declares; remove this declaration"
        status, found = n0201s(os.path.join(c_names, "owned"))
        notes = found[0].get("notes", []) if len(found) == 1 else []
        if (status != 1 or len(found) != 1 or where(found[0]) != ("src/main.cee", 4)
                or not found[0]["message"].endswith(hint) or len(found[0].get("fixes", [])) != 1
                or [(where(n), n["message"]) for n in notes] != [(("../sys/src/lib.cee", 3), "package 'sys' declares it here")]):
            problems.append(f"c-names/owned: expected N0201 at src/main.cee:4 with sys's note, hint and fix: "
                            f"{status} {found}")
        status, found = n0201s(os.path.join(c_names, "hidden"))
        if (status != 1 or len(found) != 1 or where(found[0]) != ("src/main.cee", 4) or found[0].get("fixes")
                or "make its declaration `public` in package 'sys', then use `sys.abs`" not in found[0]["message"]
                or [where(n) for n in found[0].get("notes", [])] != [("../sys/src/lib.cee", 5)]):
            problems.append(f"c-names/hidden: expected N0201 at src/main.cee:4 with the hint to make sys's abs "
                            f"public: {status} {found}")
        status, found = n0201s(os.path.join(c_names, "two-files"))
        if (status != 1 or len(found) != 1 or where(found[0]) != ("src/main.cee", 5)
                or [where(n) for n in found[0].get("notes", [])] != [("src/a.cee", 3)]):
            problems.append(f"c-names/two-files: expected N0201 at src/main.cee:5, the later file, with a note at "
                            f"src/a.cee:3: {status} {found}")
        fix = os.path.join(c_names, "fix")
        status, found = n0201s(fix, "fix")
        with open(os.path.join(fix, "src", "main.cee")) as f:
            fixed = f.read()
        if (status != 1 or sorted(where(d) for d in found) != [("src/main.cee", 5), ("src/main.cee", 8),
                                                                ("src/main.cee", 10)]
                or "fn strlen(" in fixed or "Identical to sys's" in fixed or "with its doc comment" in fixed
                or "// c" in fixed or "strcmp" in fixed or "extern \"C\" {  }" in fixed
                or "fn memcmp(a: *u8, b: *u8, n: usize) -> i32;" not in fixed or '@cname("strlen")' not in fixed
                or "fn atoi(s: *CChar) -> i64;" not in fixed):
            problems.append(f"c-names/fix: expected strlen's declaration (its doc lines and comment too) and strcmp's "
                            f"block removed, and memcmp's, length's and atoi's kept: {status} {found} {fixed!r}")
        # The owner does not depend on the order of `[dependencies]` (ADR 0081 §7): cc, mm and zz declare strlen,
        # zz depends on cc; cc owns it, and mm and zz get N0201 with a note at cc's, in every order.
        order = os.path.join(c_names, "order", "app")
        for names in itertools.permutations(["cc", "mm", "zz"]):
            with open(os.path.join(order, "ceesafe.toml"), "w") as f:
                f.write('[package]\nname = "app"\nversion = "0.1.0"\n\n[dependencies]\n'
                        + "".join(f'{n} = {{ path = "../{n}" }}\n' for n in names))
            status, found = n0201s(order)
            got = sorted((where(d), [where(n) for n in d.get("notes", [])]) for d in found)
            want = [(("../mm/src/lib.cee", 3), [("../cc/src/lib.cee", 3)]),
                    (("../zz/src/lib.cee", 3), [("../cc/src/lib.cee", 3)])]
            if status != 1 or got != want:
                problems.append(f"c-names/order {', '.join(names)}: expected N0201 in mm and zz, owned by cc: "
                                f"{status} {got}")
        app = os.path.join(c_names, "run")
        expected = "5\ntrue\n2\n7"
        p = run([compiler, "check"], app)
        if p.returncode != 0:
            problems.append(f"c-names/run: check failed: {p.stderr[:600]}")
        p = run([compiler, "run", "--interp"], app)
        if p.returncode != 0 or p.stdout.strip() != expected:
            problems.append(f"c-names/run: run --interp printed {p.stdout.strip()!r} ({p.returncode}) {p.stderr[-300:]}")
        p = run([compiler, "build"], app)
        exe = os.path.join(app, "out", "run.exe" if WINDOWS else "run")
        if p.returncode != 0:
            problems.append(f"c-names/run: build failed: {p.stderr.strip()[-600:]}")
        else:
            out = run([exe], app).stdout.strip()
            if out != expected:
                problems.append(f"c-names/run: the program printed {out!r}")

        # 16. Units that never merge, and parallel C compiles (ADR 0082 §2.1, §2.5).
        same = os.path.join(work, "same-names")
        shutil.copytree(os.path.join(ROOT, "tests", "packages", "same-names"), same)
        app = os.path.join(same, "app")
        units = ["c/alpha/src/a/mod.c", "c/alpha/src/b/mod.c", "c/alpha/src/util.c", "c/app/src/main.c",
                 "c/app/src/util.c", "c/beta/src/util.c"]
        homes = {"c/alpha/src/util.c": ["ceeF_5alpha_helper", "ceeF_5alpha_scale"], "c/beta/src/util.c": ["ceeF_4beta_helper"],
                 "c/alpha/src/a/mod.c": ["ceeF_5alpha_left"], "c/alpha/src/b/mod.c": ["ceeF_5alpha_right"],
                 "c/app/src/util.c": ["ceeF_helper"], "c/app/src/main.c": ["ceeF_main"]}

        def tree(directory):
            found = {}
            for d, _, names in os.walk(directory):
                for n in names:
                    path = os.path.join(d, n)
                    with open(path, "rb") as f:
                        found[os.path.relpath(path, directory).replace(os.sep, "/")] = f.read()
            return found

        emitted = []
        for jobs in ("1", "4"):
            out = os.path.join(same, "c-jobs" + jobs)
            p = run([compiler, "build", "--split", "--jobs", jobs, "--emit", "c", "-o", out], app)
            if p.returncode != 0:
                problems.append(f"same-names: build --split --emit c failed: {p.stderr[:600]}")
                break
            emitted.append(tree(out))
        if len(emitted) == 2:
            c = emitted[0]
            found = sorted(n for n in c if n.startswith("c/"))
            if found != units:
                problems.append(f"same-names: expected the units {units}, found {found}")
            for unit, functions in homes.items():
                text = c.get(unit, b"").decode()
                defined = [line.split("(")[0].split()[-1] for line in text.splitlines()
                           if line.startswith(("int32_t ceeF_", "void ceeF_")) and line.endswith(") {")]
                if sorted(defined) != sorted(functions):
                    problems.append(f"same-names: {unit} defines {defined}, not {functions}")
            header = c.get("ceesafe_program.h", b"").decode()
            for name in ("src/main.cee", "src/util.cee", "alpha/src/util.cee", "alpha/src/a/mod.cee", "beta/src/util.cee"):
                if f'[] = "{name}";' not in header:
                    problems.append(f"same-names: the location table does not name {name!r}")
            if emitted[0] != emitted[1]:
                problems.append("same-names: the C of --jobs 1 and --jobs 4 differ")
        printed = []
        for jobs in ("1", "4"):
            exe = os.path.join(same, "app-jobs" + jobs + (".exe" if WINDOWS else ""))
            p = run([compiler, "build", "--split", "--jobs", jobs, "-o", exe], app)
            if p.returncode != 0:
                problems.append(f"same-names: build --split --jobs {jobs} failed: {p.stderr[-600:]}")
                continue
            printed.append(run([exe], app).stdout.strip())
        if printed != ["11\n22\n300\n21"] * 2:
            problems.append(f"same-names: the programs printed {printed}")
        # What --jobs runs (ADR 0082 §2.5, §7), seen through a C compiler that logs its commands (POSIX sh): each C
        # file compiled alone with -c to its own object, the objects linked. A compile that fails exits 1, with the
        # compilers' output in the order of the files' paths although the first finish last; one output printed by
        # every compile (a bad flag's) is printed once, with a count.
        if not WINDOWS:
            wrapper = os.path.join(same, "cc-log")
            log = os.path.join(same, "cc.log")
            with open(wrapper, "w") as f:
                f.write('#!/bin/sh\n'
                        'printf "%s\\n" "$*" >> "$CEESAFE_TEST_CC_LOG"\n'
                        'if [ -n "$CEESAFE_TEST_CC_FAIL" ]; then\n'
                        '  for a in "$@"; do case "$a" in\n'
                        '    */c/alpha/*.c) sleep 1; echo "failed: $a" >&2; exit 1;;\n'
                        '    *.c) echo "failed: $a" >&2; exit 1;;\n'
                        '  esac; done\n'
                        'fi\n'
                        'exec ${CC:-cc} "$@"\n')
            os.chmod(wrapper, 0o755)
            exe = os.path.join(same, "app-logged")
            p = run([compiler, "build", "--split", "--jobs", "4", "--cc", wrapper, "-o", exe], app,
                    {"CEESAFE_TEST_CC_LOG": log})
            commands = open(log).read().splitlines() if os.path.exists(log) else []
            compiles = [c.split() for c in commands if " -c " in f" {c} "]
            sources = [next((a for a in c if a.endswith(".c")), "") for c in compiles]
            objects = [c[c.index("-o") + 1] for c in compiles if "-o" in c]
            unit_sources = sorted(s[s.rindex("/c/") + 1:] for s in sources if "/c/" in s)
            links = [c for c in commands if " -c " not in f" {c} " and all(o in c.split() for o in objects)]
            out = run([exe], app).stdout.strip() if p.returncode == 0 else ""
            if (p.returncode != 0 or unit_sources != units or len(set(objects)) != len(compiles)
                    or any(not o.endswith(".o") for o in objects) or len(links) != 1 or out != "11\n22\n300\n21"):
                problems.append(f"same-names: build --split --jobs 4 should compile each unit alone to its own object "
                                f"and link them: {p.returncode} {unit_sources} {len(objects)} objects, "
                                f"{len(links)} links, printed {out!r} {p.stderr[-300:]}")
            p = run([compiler, "build", "--split", "--jobs", "4", "--cc", wrapper, "-o", exe], app,
                    {"CEESAFE_TEST_CC_LOG": log, "CEESAFE_TEST_CC_FAIL": "1"})
            failed = [line[len("failed: "):] for line in p.stderr.splitlines() if line.startswith("failed: ")]
            names = [os.path.relpath(f, os.path.commonpath(failed)).replace(os.sep, "/") for f in failed] if failed else []
            if p.returncode != 1 or len(failed) < len(units) or names != sorted(names):
                problems.append(f"same-names: a failing split build should exit 1 with the compilers' output in the "
                                f"order of the C files' paths: {p.returncode} {names} {p.stderr[-300:]}")
            p = run([compiler, "build", "--split", "--jobs", "4", "--cflag", "-fno-such-flag-ceesafe", "-o", exe], app)
            if p.returncode != 1 or "the C compiler printed the same for " not in p.stderr:
                problems.append(f"same-names: a bad --cflag should fail the build (1) and its message be printed "
                                f"once, with a count: {p.returncode} {p.stderr[-400:]}")
        # build --jobs without --split is a usage error (ADR 0082 §2.5).
        p = run([compiler, "build", "--jobs", "4", "--emit", "c", "-o", os.path.join(same, "c-nosplit")], app)
        if p.returncode != 2 or "--jobs needs --split" not in p.stderr:
            problems.append(f"same-names: build --jobs without --split should be a usage error (2): {p.returncode} "
                            f"{p.stderr[:200]}")
        util = os.path.join(same, "alpha", "src", "util.cee")
        with open(util) as f:
            text = f.read()
        with open(util, "w") as f:
            f.write(text.replace("x + 10;", "x + true;"))
        # The whole program's check, which reports the dependency's own error (`check` alone reports N0364 for it).
        p = run([compiler, "check", "--whole-program", "--diagnostics-format=json"], app)
        try:
            found = [(d["code"], d["file"], d.get("package")) for d in json.loads(p.stdout)["diagnostics"]]
        except (ValueError, KeyError):
            found = []
        if found != [("N0205", "../alpha/src/util.cee", "alpha")] or not os.path.isfile(os.path.join(app, found[0][1])):
            problems.append(f"same-names: expected N0205 in ../alpha/src/util.cee of package alpha: {found} "
                            f"{p.stdout[:300]}{p.stderr[:300]}")
        with open(util, "w") as f:
            f.write(text)
        clash = os.path.join(work, "case-clash")
        for d, name, body in (("x", "Util.cee", "fn main() { Console.writeLine(twice(2)); }\n"),
                              ("y", "util.cee", "fn twice(n: i32) -> i32 => n * 2;\n")):
            os.makedirs(os.path.join(clash, d))
            with open(os.path.join(clash, d, name), "w") as f:
                f.write(body)
        os.makedirs(os.path.join(clash, "here"))
        files = [os.path.join(clash, "x", "Util.cee"), os.path.join(clash, "y", "util.cee")]
        p = run([compiler, "build", "--split", "--emit", "c", "--diagnostics-format=json", "-o",
                 os.path.join(clash, "c"), *files], os.path.join(clash, "here"))
        try:
            found = [(d["code"], d["file"]) for d in json.loads(p.stdout)["diagnostics"]]
        except (ValueError, KeyError):
            found = []
        if p.returncode != 1 or found != [("N0363", "util.cee")]:
            problems.append(f"same-names: expected N0363 at util.cee: {p.returncode} {found} {p.stderr[:300]}")
        p = run([compiler, "build", "--emit", "c", "-o", os.path.join(clash, "one"), *files], os.path.join(clash, "here"))
        if p.returncode != 0:
            problems.append(f"same-names: the same files without --split failed: {p.stderr[:300]}")
        # Files whose units would have one path are not N0363 (ADR 0082 §7): two files outside the working directory
        # with one name, and `../x.cee` beside `__/x.cee`; the later gets `-2`, and the program runs.
        same_path = os.path.join(work, "same-path")
        for d, name, body in (("p", "util.cee", "fn helperP() -> i32 => 1;\n"),
                              ("q", "util.cee", "fn helperQ() -> i32 => 2;\n"),
                              ("w/sub", "main.cee", "fn main() { Console.writeLine(helperP() + helperQ()); }\n"),
                              (".", "x.cee", "fn helperA() -> i32 => 3;\n"),
                              ("w/__", "x.cee", "fn helperB() -> i32 => 4;\n"),
                              ("w", "main.cee", "fn main() { Console.writeLine(helperA() + helperB()); }\n")):
            os.makedirs(os.path.join(same_path, d), exist_ok=True)
            with open(os.path.join(same_path, d, name), "w") as f:
                f.write(body)
        w = os.path.join(same_path, "w")
        for files, want_units, want_out in (
                ([os.path.join(same_path, "p", "util.cee"), os.path.join(same_path, "q", "util.cee"), "sub/main.cee"],
                 ["c/util/sub/main.c", "c/util/util-2.c", "c/util/util.c"], "3"),
                (["main.cee", "../x.cee", "__/x.cee"], ["c/main/__/x-2.c", "c/main/__/x.c", "c/main/main.c"], "7")):
            cdir = os.path.join(same_path, "c-" + want_out)
            p = run([compiler, "build", "--split", "--emit", "c", "-o", cdir, *files], w)
            found = sorted(n for n in tree(cdir) if n.startswith("c/")) if p.returncode == 0 else []
            exe = os.path.join(same_path, "prog-" + want_out + (".exe" if WINDOWS else ""))
            q = run([compiler, "build", "--split", "--jobs", "4", "-o", exe, *files], w)
            out = run([exe], w).stdout.strip() if q.returncode == 0 else ""
            if found != want_units or out != want_out:
                problems.append(f"same-names: {files} should make the units {want_units} and print {want_out}: "
                                f"{found} {out!r} {p.stderr[:300]}{q.stderr[:300]}")
        # On a file system that keeps letter case apart (Linux's), the same in a package.
        with open(os.path.join(app, "src", "Util.cee"), "w") as f:
            f.write("fn unused() -> i32 => 1;\n")
        if sorted(n for n in os.listdir(os.path.join(app, "src")) if n.lower() == "util.cee") == ["Util.cee", "util.cee"]:
            p = run([compiler, "build", "--split", "--emit", "c", "--diagnostics-format=json", "-o",
                     os.path.join(same, "c-clash")], app)
            try:
                found = [(d["code"], d["file"], d.get("package")) for d in json.loads(p.stdout)["diagnostics"]]
            except (ValueError, KeyError):
                found = []
            if p.returncode != 1 or found != [("N0363", "src/util.cee", "app")]:
                problems.append(f"same-names: expected N0363 at src/util.cee: {p.returncode} {found} {p.stderr[:300]}")

        # 17. An interface's associated types and constants in `ceesafec doc`.
        docs = os.path.join(work, "doc-interface")
        os.makedirs(docs)
        with open(os.path.join(docs, "kind.cee"), "w") as f:
            f.write("/// A kind.\npublic interface Kind {\n    const CODE: u32;\n    type Item: Copy;\n"
                    "    const FLAG: bool = false;\n    fn id(borrow this) -> u32;\n}\n")
        p = run([compiler, "doc", "kind.cee"], docs)
        want = ("public interface Kind {\n    const CODE: u32;\n    type Item: Copy;\n    const FLAG: bool = false;\n"
                "    fn id(borrow this) -> u32;\n}\n")
        if p.returncode != 0 or want not in p.stdout:
            problems.append(f"doc: expected the interface with its constants and types:\n{p.stdout}{p.stderr[:300]}")

        # 18. Embedded files (ADR 0087 §2.8) and constant `+` across packages (§2.7).
        embed = os.path.join(work, "embed")
        shutil.copytree(os.path.join(ROOT, "tests", "packages", "embed"), embed)
        app = os.path.join(embed, "app")
        escape = os.path.join(embed, "escape")
        try:
            os.symlink("greeting.txt", os.path.join(app, "assets", "alias.txt"))
            os.symlink(os.path.join("..", "..", "outside", "secret.txt"), os.path.join(escape, "assets", "leak.txt"))
            os.symlink(os.path.join("..", "..", "outside"), os.path.join(escape, "assets", "linked"),
                       target_is_directory=True)
            os.symlink("loop", os.path.join(escape, "assets", "loop"))
        except (OSError, NotImplementedError):
            shutil.copy(os.path.join(app, "assets", "greeting.txt"), os.path.join(app, "assets", "alias.txt"))
        with open(os.path.join(app, "assets", "changing.txt"), "w") as f:
            f.write("one")
        with open(os.path.join(escape, "assets", "latin1.txt"), "wb") as f:
            f.write(b"caf\xe9\n")
        with open(os.path.join(escape, "assets", "limit.bin"), "wb") as f:
            f.write(b"\x00" * 16777216)
        with open(os.path.join(escape, "assets", "over.bin"), "wb") as f:
            f.write(b"\x00" * 16777217)
        p = run([compiler, "fetch"], app)
        p = run([compiler, "check"], app)
        if p.returncode != 0:
            problems.append(f"embed: app did not check: {p.stdout[:300]}{p.stderr[:600]}")
        with open(os.path.join(app, "assets", "changing.txt"), "w") as f:
            f.write("two")
        expected = "hello from a file\ntrue\n0\ntwo\ntrue\nwords: joined\nbanner!\nwords: c\nbanner"
        p = run([compiler, "run", "--interp"], app)
        if p.returncode != 0 or p.stdout.strip() != expected:
            problems.append(f"embed: run --interp printed {p.stdout.strip()!r} ({p.returncode}) {p.stderr[-600:]}")
        p = run([compiler, "build"], app)
        exe = os.path.join(app, "out", "app.exe" if WINDOWS else "app")
        if p.returncode != 0:
            problems.append(f"embed: build failed: {p.stderr.strip()[-600:]}")
        else:
            out = run([exe], app).stdout.strip()
            if out != expected:
                problems.append(f"embed: the program printed {out!r}")
        p = run([compiler, "check", "--diagnostics-format=json"], escape)
        try:
            found = sorted((d["start"]["line"], d["code"]) for d in json.loads(p.stdout)["diagnostics"])
        except (ValueError, KeyError):
            found = []
        # Without links, lines 3-5 name files that are not there: N0362 too.
        want = [(3, "N0362"), (4, "N0362"), (5, "N0362"), (6, "N0361"), (9, "N0362"), (10, "N0362")]
        if p.returncode != 1 or found != want:
            problems.append(f"embed: escape: expected {want}, found {found} ({p.returncode}) {p.stderr[:300]}")

        # 19. Embedded bytes in C and in the interpreter: large, small and empty files.
        large = os.path.join(work, "embed-large")
        os.makedirs(os.path.join(large, "src"))
        rng = random.Random(87)
        big = bytes(rng.randrange(256) for _ in range(3 * 1024 * 1024 + 17))
        small = b"\xff\xfe\x00\x01"
        for name, data in (("big.bin", big), ("small.bin", small), ("empty.bin", b"")):
            with open(os.path.join(large, "src", name), "wb") as f:
                f.write(data)
        with open(os.path.join(large, "ceesafe.toml"), "w") as f:
            f.write('[package]\nname = "large"\n')
        with open(os.path.join(large, "src", "main.cee"), "w") as f:
            f.write('static BIG: Slice<u8> = embed("big.bin");\nconst SAME: Slice<u8> = embed("./big.bin");\n'
                    'static SMALL: Slice<u8> = embed("small.bin");\nconst EMPTY: Slice<u8> = embed("empty.bin");\n\n'
                    'fn sum(bytes: Slice<u8>) -> u64 {\n    var total: u64 = 0;\n    for (b in bytes) {\n'
                    '        total = (total * 31 + b as u64) % 1000000007;\n    }\n    return total;\n}\n\n'
                    'fn main() {\n    let local = embed("big.bin");\n    Console.writeLine(BIG.length);\n'
                    '    Console.writeLine(sum(BIG));\n    Console.writeLine(SAME.length == local.length);\n'
                    '    Console.writeLine(sum(SMALL));\n    Console.writeLine(EMPTY.length);\n}\n')
        def checksum(data):
            total = 0
            for b in data:
                total = (total * 31 + b) % 1000000007
            return total
        expected = f"{len(big)}\n{checksum(big)}\ntrue\n{checksum(small)}\n0"
        p = run([compiler, "build"], large)
        exe = os.path.join(large, "out", "large.exe" if WINDOWS else "large")
        if p.returncode != 0:
            problems.append(f"embed-large: build failed: {p.stderr.strip()[-600:]}")
        else:
            out = run([exe], large).stdout.strip()
            if out != expected:
                problems.append(f"embed-large: the program printed {out!r}, not {expected!r}")
        p = run([compiler, "run", "--interp"], large)
        if p.returncode != 0 or p.stdout.strip() != expected:
            problems.append(f"embed-large: run --interp printed {p.stdout.strip()!r} ({p.returncode}) {p.stderr[-300:]}")
        cdir = os.path.join(work, "embed-large-c")
        p = run([compiler, "build", "--emit", "c", "-o", cdir], large)
        try:
            text = open(os.path.join(cdir, "main.c")).read()
        except OSError:
            text = ""
        arrays = [line for line in text.splitlines() if line.startswith("static CEE_UNUSED const uint8_t ceeLS_")]
        if p.returncode != 0 or len(arrays) != 2 or not any(l.endswith("[] = {0xFF, 0xFE, 0x00, 0x01};")
                                                            for l in arrays):
            problems.append(f"embed-large: --emit c: expected two byte arrays, the short one among them: "
                            f"{[a[:60] for a in arrays]} ({p.returncode}) {p.stderr[-300:]}")
        # Split: each array is defined once, in ceesafe_instances.c, and declared in the header every unit includes.
        sdir = os.path.join(work, "embed-large-split")
        p = run([compiler, "build", "--split", "--emit", "c", "-o", sdir], large)
        definitions, declarations = 0, 0
        for top, _, files in os.walk(sdir):
            for name in files:
                if name.endswith((".c", ".h")):
                    for line in open(os.path.join(top, name), errors="replace"):
                        definitions += line.startswith("const uint8_t ceeLS_") or "CEE_UNUSED const uint8_t ceeLS_" in line
                        declarations += line.startswith("extern const uint8_t ceeLS_")
        if p.returncode != 0 or definitions != 2 or declarations != 2:
            problems.append(f"embed-large: --split --emit c: expected two arrays defined once and declared once: "
                            f"{definitions} definitions, {declarations} declarations ({p.returncode}) {p.stderr[-300:]}")
        split_exe = os.path.join(work, "embed-large-split-program.exe" if WINDOWS else "embed-large-split-program")
        p = run([compiler, "build", "--split", "-o", split_exe], large)
        if p.returncode != 0 or run([split_exe], large).stdout.strip() != expected:
            problems.append(f"embed-large: build --split: the program did not print {expected!r} {p.stderr[-300:]}")
        elif os.path.getsize(split_exe) > os.path.getsize(exe) + len(big) // 2:
            problems.append(f"embed-large: build --split: {os.path.getsize(split_exe)} bytes, against "
                            f"{os.path.getsize(exe)} without --split: an embedded file is in it more than once")

        # 20. [embed] roots: outside the package for the root package and a path dependency, not for a git one.
        roots = os.path.join(work, "roots")
        def write(path, text):
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as f:
                f.write(text)
        shared = os.path.join(roots, "shared")
        write(os.path.join(shared, "data.txt"), "shared")
        write(os.path.join(roots, "shared2", "two.txt"), "two")
        try:
            os.makedirs(os.path.join(roots, "app"))
            os.symlink(os.path.join("..", "shared2"), os.path.join(roots, "app", "gen"), target_is_directory=True)
            linked = True
        except (OSError, NotImplementedError):
            write(os.path.join(roots, "app", "gen", "two.txt"), "two")
            linked = False
        write(os.path.join(roots, "near", "ceesafe.toml"),
              '[package]\nname = "near"\nkind = "library"\n\n[embed]\nroots = ["../shared"]\n')
        write(os.path.join(roots, "near", "src", "lib.cee"),
              'public const TEXT: StringView = "near " + embedText("../../shared/data.txt");\n')
        def repository(name, source):
            directory = os.path.join(roots, name)
            write(os.path.join(directory, "ceesafe.toml"),
                  f'[package]\nname = "{name}"\nkind = "library"\n\n[embed]\nroots = ["linked", "../shared"]\n')
            write(os.path.join(directory, "own.txt"), "own")
            write(os.path.join(directory, "src", "lib.cee"), source)
            if linked:
                os.symlink(shared, os.path.join(directory, "linked"), target_is_directory=True)
            run(git + ["init", "-q"], directory)
            run(git + ["add", "-A"], directory)
            run(git + ["commit", "-qm", "one"], directory)
            commit = run(["git", "rev-parse", "HEAD"], directory).stdout.strip()
            return f'{{ git = "{directory.replace(chr(92), "/")}", rev = "{commit}" }}'
        far = repository("far", 'public const OWN: StringView = embedText("../own.txt");\n')
        farbad = repository("farbad", 'public const LINKED: StringView = embedText("../linked/data.txt");\n'
                                      'public const SHARED: StringView = embedText("../../shared/data.txt");\n')
        write(os.path.join(roots, "app", "ceesafe.toml"),
              '[package]\nname = "app"\n\n[embed]\nroots = ["../shared", "gen"]\n\n[dependencies]\n'
              f'near = {{ path = "../near" }}\nfar = {far}\n')
        write(os.path.join(roots, "app", "src", "main.cee"),
              'const SHARED: StringView = embedText("../../shared/data.txt");\n'
              'const TWO: StringView = embedText("../gen/two.txt");\n\nfn main() {\n    Console.writeLine(SHARED);\n'
              '    Console.writeLine(TWO);\n    Console.writeLine(near.TEXT);\n    Console.writeLine(far.OWN);\n}\n')
        app = os.path.join(roots, "app")
        run([compiler, "fetch"], app)
        p = run([compiler, "run", "--interp"], app)
        if p.returncode != 0 or p.stdout.strip() != "shared\ntwo\nnear shared\nown":
            problems.append(f"embed roots: app printed {p.stdout.strip()!r} ({p.returncode}) {p.stderr[-600:]}")
        bad = os.path.join(roots, "bad")
        write(os.path.join(bad, "ceesafe.toml"), f'[package]\nname = "bad"\n\n[dependencies]\nfarbad = {farbad}\n')
        write(os.path.join(bad, "src", "main.cee"), "fn main() {\n    Console.writeLine(farbad.SHARED);\n}\n")
        run([compiler, "fetch"], bad)
        # The dependency's own diagnostics are the whole program's check's; `check` gives one N0364 for them.
        p = run([compiler, "check", "--whole-program", "--diagnostics-format=json"], bad)
        try:
            found = sorted((d["code"], d["start"]["line"]) for d in json.loads(p.stdout)["diagnostics"])
        except (ValueError, KeyError):
            found = []
        if p.returncode != 1 or found != [("N0362", 1), ("N0362", 2)]:
            problems.append(f"embed roots: farbad: expected N0362 on lines 1 and 2: {found} ({p.returncode}) "
                            f"{p.stdout[:400]}{p.stderr[:400]}")
        p = run([compiler, "check", "--diagnostics-format=json"], bad)
        if p.returncode != 1 or '"N0364"' not in p.stdout or "N0362" not in p.stdout:
            problems.append(f"embed roots: farbad: check gave no N0364 noting the N0362: {p.stdout[:400]}")
        errors = os.path.join(roots, "errors")
        write(os.path.join(errors, "ceesafe.toml"),
              '[package]\nname = "errors"\n\n[embed]\nroots = ["/abs", "a\\\\b", "$X/y", "fine"]\nother = "x"\n')
        write(os.path.join(errors, "src", "main.cee"), "fn main() {}\n")
        p = run([compiler, "check", "--diagnostics-format=json"], errors)
        try:
            found = [d["code"] for d in json.loads(p.stdout)["diagnostics"]]
        except (ValueError, KeyError):
            found = p.stdout + p.stderr
        if p.returncode == 0 or found != ["N0321"] * 4:
            problems.append(f"embed roots: expected four N0321 for the invalid roots and field: {found}")

        # 21. Embed security: nothing outside reaches a package through a git dependency, a checkout, or a race.
        security = os.path.join(work, "embed-security")
        shutil.copytree(os.path.join(ROOT, "tests", "packages", "embed-security"), security)
        os.makedirs(os.path.join(work, "secret"))
        with open(os.path.join(work, "secret", "token.txt"), "w") as f:
            f.write("TOPSECRET\n")
        def committed(name):
            directory = os.path.join(security, name)
            run(git + ["init", "-q"], directory)
            run(git + ["add", "-A"], directory)
            run(git + ["commit", "-qm", "one"], directory)
            commit = run(["git", "rev-parse", "HEAD"], directory).stdout.strip()
            return f'{{ git = "{directory.replace(chr(92), "/")}", rev = "{commit}" }}'
        def victim(name, dependency):
            directory = os.path.join(security, "victim-" + dependency)
            os.makedirs(os.path.join(directory, "src"))
            with open(os.path.join(directory, "ceesafe.toml"), "w") as f:
                f.write(f'[package]\nname = "{name}"\n\n[dependencies]\n{dependency} = {committed(dependency)}\n')
            with open(os.path.join(directory, "src", "main.cee"), "w") as f:
                f.write(f"fn main() {{\n    Console.writeLine({dependency}.STOLEN);\n}}\n")
            run([compiler, "fetch"], directory)
            return directory
        def diagnostics(p):
            try:
                return [(d["code"], d["message"]) for d in json.loads(p.stdout)["diagnostics"]]
            except (ValueError, KeyError):
                return []
        shutil.move(os.path.join(security, "outside-inner"), os.path.join(work, "outside-inner"))
        for dependency in ("evil", "evildeep", "evilroots", "evilout"):
            directory = victim("victim", dependency)
            if dependency == "evilout":
                # A git package's path dependency is a directory of its checkout (ADR 0152): one out of it is N0322.
                p = run([compiler, "fetch"], directory)
                if p.returncode != 1 or "error[N0322]" not in p.stderr or "outside-inner" not in p.stderr:
                    problems.append(f"embed security: evilout: fetch: expected N0322 at its path: {p.stderr[:400]}")
                continue
            p = run([compiler, "check", "--whole-program", "--diagnostics-format=json"], directory)
            found = diagnostics(p)
            if p.returncode != 1 or [c for c, _ in found] != ["N0362"] or "TOPSECRET" in p.stdout + p.stderr:
                problems.append(f"embed security: {dependency}: expected one N0362: {found} ({p.returncode}) "
                                f"{p.stderr[:300]}")
            p = run([compiler, "check", "--diagnostics-format=json"], directory)
            found = diagnostics(p)
            if p.returncode != 1 or [c for c, _ in found] != ["N0364"] or "TOPSECRET" in p.stdout + p.stderr or \
                    "N0362" not in p.stdout:
                problems.append(f"embed security: {dependency}: check: expected one N0364 noting the N0362: {found}")
            p = run([compiler, "run", "--interp"], directory)
            if p.returncode == 0 or "TOPSECRET" in p.stdout + p.stderr:
                problems.append(f"embed security: {dependency}: run --interp read the secret ({p.returncode})")
        home = os.environ["CEESAFE_HOME"]
        def checkout_of(name):
            for top, _, files in os.walk(os.path.join(home, "git", "checkouts")):
                if "ceesafe.toml" in files and f'name = "{name}"' in open(os.path.join(top, "ceesafe.toml")).read():
                    return top
            return None
        checkout = checkout_of("evilroots")
        outer = checkout_of("evilout")
        if outer is None:
            problems.append("embed security: evilout has no checkout in the package cache")
        else:
            # The checkout as the root package: its path dependency outside the cache is kept inside the checkout.
            p = run([compiler, "check", "--whole-program", "--diagnostics-format=json"], outer)
            found = diagnostics(p)
            if p.returncode != 1 or [c for c, _ in found] != ["N0362"] or "TOPSECRET" in p.stdout + p.stderr:
                problems.append(f"embed security: check in the evilout checkout: expected one N0362: {found} "
                                f"({p.returncode}) {p.stderr[:300]}")
        if checkout is None:
            problems.append("embed security: evilroots has no checkout in the package cache")
        else:
            p = run([compiler, "check", "--diagnostics-format=json"], checkout)
            found = diagnostics(p)
            if p.returncode != 1 or [c for c, _ in found] != ["N0362"] or "TOPSECRET" in p.stdout + p.stderr:
                problems.append(f"embed security: check in the evilroots checkout: expected one N0362: {found} "
                                f"({p.returncode})")
            published = language_server_diagnostics(compiler, os.path.join(checkout, "src", "lib.cee"))
            if "N0362" not in published or "TOPSECRET" in published:
                problems.append(f"embed security: the language server in the evilroots checkout: {published[:400]}")
        local = os.path.join(security, "local")
        secret_directory = os.path.join(work, "secret")
        made_fifo = False
        try:
            os.symlink(os.path.join(secret_directory, "token.txt"), os.path.join(local, "assets", "link.txt"))
        except (OSError, NotImplementedError):
            pass  # without the privilege to make links, the line names a missing file: the same N0362
        if hasattr(os, "mkfifo"):
            os.mkfifo(os.path.join(local, "assets", "fifo"))
            made_fifo = True
        p = run([compiler, "check", "--diagnostics-format=json"], local)
        found = diagnostics(p)
        messages = [m for _, m in found]
        same = len(messages) >= 2 and messages[0].replace("link.txt", "missing.txt") == messages[1]
        hidden = all("secret" not in m and "token" not in m for m in messages)
        fifo_refused = not made_fifo or (len(messages) == 3 and "not a regular file" in messages[2])
        if p.returncode != 1 or [c for c, _ in found] != ["N0362"] * 3 or not same or not hidden or not fifo_refused:
            problems.append(f"embed security: local: expected three N0362, the first two alike and naming nothing "
                            f"resolved, the FIFO's not a regular file: {found} ({p.returncode})")
        if made_fifo:
            published = language_server_diagnostics(compiler, os.path.join(local, "src", "main.cee"))
            if published.count("N0362") != 3:
                problems.append(f"embed security: the language server with a FIFO: {published[:400]}")

        # 22. The language server's hover in a constant's and a static's value (ADR 0087 §2.8).
        hovered = os.path.join(work, "hover")
        os.makedirs(os.path.join(hovered, "src"))
        with open(os.path.join(hovered, "ceesafe.toml"), "w") as f:
            f.write('[package]\nname = "hover"\n')
        with open(os.path.join(hovered, "src", "note.txt"), "w") as f:
            f.write("note\n")
        with open(os.path.join(hovered, "src", "main.cee"), "w") as f:
            f.write('const NOTE: StringView = embedText("note.txt");\nstatic BYTES: Slice<u8> = embed("note.txt");\n\n'
                    'fn main() {\n    Console.write(NOTE);\n    Console.writeLine(BYTES.length);\n}\n')
        source = os.path.join(hovered, "src", "main.cee")
        for line, character, want in ((0, 30, "StringView"), (1, 30, "Slice<u8>")):
            _, answered = language_server(compiler, source, (line, character))
            if want not in answered:
                problems.append(f"hover: {line}:{character} should show {want}: {answered[:300]!r}")

        # 27. A dependency's @repr types keep their declared C names (type-system.md §28.5; ADR 0081 §8).
        repr = os.path.join(work, "repr-dep")
        shutil.copytree(os.path.join(ROOT, "tests", "packages", "repr-dep"), repr)
        app = os.path.join(repr, "app")
        expected = "8\n40\n42\n42\n42\n44\n55"
        p = run([compiler, "check"], app)
        if p.returncode != 0:
            problems.append(f"repr-dep: check failed: {p.stderr[:600]}")
        exe = os.path.join(work, "repr-dep-out", "app.exe" if WINDOWS else "app")
        builds = [("one file", []), ("--split", ["--split"])]
        if not WINDOWS:
            builds.append(("--emit obj", ["--emit", "obj"]))
        for label, flags in builds:
            target = exe + (".o" if "obj" in flags else "")
            p = run([compiler, "build"] + flags + ["-o", target], app)
            if p.returncode != 0:
                problems.append(f"repr-dep {label}: build failed: {p.stderr.strip()[-600:]}")
                continue
            if "obj" in flags:
                p = run([os.environ.get("CC", "cc"), target, "-o", exe], app)
                if p.returncode != 0:
                    problems.append(f"repr-dep {label}: linking the object failed: {p.stderr.strip()[-600:]}")
                    continue
            out = run([exe], app).stdout.strip()
            if out != expected:
                problems.append(f"repr-dep {label}: the program printed {out!r}, not {expected!r}")
            os.remove(exe)
        for label, flags in (("--emit c", []), ("--split --emit c", ["--split"])):
            cdir = os.path.join(work, "repr-dep-c" + "".join(flags))
            p = run([compiler, "build", "--emit", "c"] + flags + ["-o", cdir], app)
            if p.returncode != 0:
                problems.append(f"repr-dep {label}: failed: {p.stderr.strip()[-600:]}")
                continue
            with open(os.path.join(cdir, "ceesafe_exports.h")) as f:
                header = f.read()
            for want in ("struct Point {", "struct Rect {", "struct Path {", "struct Spot {", "typedef uint8_t Shade;",
                         "Shade_Dark = 1", "int32_t geo_area(struct Rect, Shade);",
                         "int32_t app_measure(struct Point, Shade);"):
                if want not in header:
                    problems.append(f"repr-dep {label}: ceesafe_exports.h lacks {want!r}")
            for top, _, files in os.walk(cdir):
                for name in files:
                    if not name.endswith((".c", ".h")) or name.startswith("ceesafe_rt") or top.endswith("ceesafe"):
                        continue
                    with open(os.path.join(top, name)) as f:
                        for line in f:
                            code = line.split('"')[0]  # the layout assertions' messages name geo.Point
                            if "geo." in code or "other." in code:
                                problems.append(f"repr-dep {label}: {name} names a type with its package: "
                                                f"{line.strip()[:200]}")
                                break
        # 28. A C++ caller includes ceesafe_exports.h and links with the program (ADR 0088 §2.6).
        cxx = os.environ.get("CXX", "c++")
        if not WINDOWS and shutil.which(cxx):
            cdir = os.path.join(work, "repr-dep-c")
            source = os.path.join(work, "repr-dep-cxx.cpp")
            with open(source, "w") as f:
                f.write('#include "ceesafe_exports.h"\n'
                        "static int32_t sum(struct Point p) { return p.x + p.y; }\n"
                        'extern "C" int cxx_total(void) {\n'
                        "    Point p{3, 4};\n"
                        "    Rect r{{0, 0}, p};\n"
                        "    return app_measure(p, Shade_Dark) + geo_area(r, Shade_Light) + geo_apply(sum) +\n"
                        "        (int)geo_corners;\n"
                        "}\n")
            objects = os.path.join(work, "repr-dep-cxx")
            os.makedirs(objects, exist_ok=True)
            program = os.path.join(objects, "app.o")
            p = run([compiler, "build", "--emit", "obj", "-o", program], app)
            q = run([cxx, "-std=c++11", "-Wall", "-Werror", "-I", cdir, "-c", source, "-o",
                     os.path.join(objects, "cxx.o")], app)
            if p.returncode != 0 or q.returncode != 0:
                problems.append(f"repr-dep C++: {p.stderr.strip()[-300:]} {q.stderr.strip()[-600:]}")
            else:
                linked = os.path.join(objects, "app")
                p = run([cxx, program, os.path.join(objects, "cxx.o"), "-o", linked], app)
                out = run([linked], app).stdout.strip() if p.returncode == 0 else None
                if p.returncode != 0 or out != expected:
                    problems.append(f"repr-dep C++: the link failed or the program printed {out!r}: "
                                    f"{p.stderr.strip()[-600:]}")

        clash = os.path.join(repr, "clash")
        p = run([compiler, "check", "--whole-program", "--diagnostics-format=json"], clash)
        try:
            found = [d for d in json.loads(p.stdout)["diagnostics"] if d["code"] == "N0201"]
        except (ValueError, KeyError):
            found = [{"unparsed": p.stdout[:300] + p.stderr[:300]}]
        got = sorted((where(d), [where(n) for n in d.get("notes", [])]) for d in found)
        want = [(("../rival/src/lib.cee", 5), [("../geo/src/lib.cee", 6)]),
                (("../rival/src/lib.cee", 10), [("../geo/src/lib.cee", 31)]),
                (("src/main.cee", 5), [("../geo/src/lib.cee", 13)])]
        if p.returncode != 1 or got != want or not all("§28.5" in d.get("message", "") for d in found):
            problems.append(f"repr-dep/clash: expected N0201 at rival's Point and Shade and app's Rect, owned by geo: "
                            f"{p.returncode} {got}")
        # The per-package check: rival's clash needs geo, which rival does not depend on, so N0364 names geo and
        # suggests the whole program's check, not `check --package rival`, which finds nothing.
        p = run([compiler, "check", "--diagnostics-format=json"], clash)
        try:
            n0364 = [d["message"] for d in json.loads(p.stdout)["diagnostics"] if d["code"] == "N0364"]
        except (ValueError, KeyError):
            n0364 = [p.stdout[:300] + p.stderr[:300]]
        if p.returncode != 1 or len(n0364) != 1 or "'rival'" not in n0364[0] or "one with 'geo'" not in n0364[0] or \
                "check --whole-program'" not in n0364[0]:
            problems.append(f"repr-dep/clash: N0364 should name geo and suggest --whole-program: {n0364}")
        p = run([compiler, "check", "--package", "rival"], clash)
        if p.returncode != 0:
            problems.append(f"repr-dep/clash: check --package rival ({p.returncode}) {p.stderr[-300:]}")

        # 29. A dependency's interface named in an impl (ADR 0097).
        qualified = os.path.join(work, "qualified-impls")
        shutil.copytree(os.path.join(ROOT, "tests", "packages", "qualified-impls"), qualified)
        app = os.path.join(qualified, "app")
        expected = "12\n12\n12\n24\n60\nsquare\n33"
        p = run([compiler, "check", "--diagnostics-format=json"], app)
        try:
            found = [d["code"] for d in json.loads(p.stdout)["diagnostics"]]
        except (ValueError, KeyError):
            found = [p.stdout[:300] + p.stderr[:300]]
        if p.returncode != 0 or found:
            problems.append(f"qualified-impls: check should be clean: {p.returncode} {found}")
        p = run([compiler, "run", "--interp"], app)
        if p.returncode != 0 or p.stdout.strip() != expected:
            problems.append(f"qualified-impls: run --interp printed {p.stdout.strip()!r} ({p.returncode}) "
                            f"{p.stderr[-300:]}")
        p = run([compiler, "build"], app)
        exe = os.path.join(app, "out", "app.exe" if WINDOWS else "app")
        if p.returncode != 0:
            problems.append(f"qualified-impls: build failed: {p.stderr.strip()[-600:]}")
        else:
            out = run([exe], app).stdout.strip()
            if out != expected:
                problems.append(f"qualified-impls: the program printed {out!r}")
        for name, code in (("orphan", "N0267"), ("missing", "N0311"), ("parameter", "N0315")):
            p = run([compiler, "check", "--diagnostics-format=json"], os.path.join(qualified, name))
            try:
                found = [d["code"] for d in json.loads(p.stdout)["diagnostics"]]
            except (ValueError, KeyError):
                found = []
            if p.returncode != 1 or found != [code]:
                problems.append(f"qualified-impls/{name}: expected {code} alone: {p.returncode} {found} "
                                f"{p.stderr[:300]}")

        # 30. A @constant body's error is reported in the body's file (type-system.md §39.4).
        constant = os.path.join(work, "constant-other-file")
        shutil.copytree(os.path.join(ROOT, "tests", "packages", "constant-other-file"), constant)
        p = run([compiler, "check", "--diagnostics-format=json"], constant)
        try:
            found = [(d["code"], os.path.basename(d["file"]), d["start"]["line"])
                     for d in json.loads(p.stdout)["diagnostics"]]
        except (ValueError, KeyError):
            found = [p.stdout[:300] + p.stderr[:300]]
        if p.returncode != 1 or found != [("N0338", "zother.cee", 12)]:
            problems.append(f"constant-other-file: expected N0338 at zother.cee:12 alone: {p.returncode} {found}")

        # 31. A private item is usable in its own file only (type-system.md §2, ADR 0109).
        private = os.path.join(work, "private-items")
        shutil.copytree(os.path.join(ROOT, "tests", "packages", "private-items"), private)
        main_path = os.path.join(private, "src", "main.cee")
        with open(main_path, encoding="utf-8") as f:
            main_lines = f.read().split("\n")
        marked = [k + 1 for k, line in enumerate(main_lines) if line.endswith("// N0296")]
        # A struct with a drop block is taken apart only in the file that declares it (ADR 0123 §2.6).
        apart = [k + 1 for k, line in enumerate(main_lines) if line.endswith("// N0252")]
        twice = [k + 1 for k, line in enumerate(main_lines) if "Kind.Two(LIMIT)" in line]
        p = run([compiler, "check", "--diagnostics-format=json"], private)
        try:
            found = sorted((d["code"], os.path.basename(d["file"]), d["start"]["line"])
                           for d in json.loads(p.stdout)["diagnostics"])
        except (ValueError, KeyError):
            found = [p.stdout[:300] + p.stderr[:300]]
        expected = sorted([("N0296", "main.cee", line) for line in marked + twice]
                          + [("N0252", "main.cee", line) for line in apart])
        if p.returncode != 1 or len(marked) != 30 or len(apart) != 1 or found != expected:
            problems.append(f"private-items: expected N0296 at main.cee's {len(marked)} marked lines "
                            f"(line {twice} twice) and N0252 at {apart}: {p.returncode} {found}")
        for name, where in (("'helper'", "src/shapes.cee"), ("'Open.mine'", "src/more.cee")):
            if name + " is private to " + where not in p.stdout:
                problems.append(f"private-items: no message says {name} is private to {where}: {p.stdout[:300]}")
        with open(main_path, "w", encoding="utf-8") as f:
            f.write("\n".join(line for line in main_lines if not line.endswith(("// N0296", "// N0252"))))
        p = run([compiler, "run", "--interp"], private)
        if p.returncode != 0 or p.stdout.split() != ["31", "11"]:
            problems.append(f"private-items: without the marked lines the program should print 31 and 11: "
                            f"{p.returncode} {p.stdout!r} {p.stderr[-300:]}")

        impl_bounds_order(compiler, work, problems)
        region_bounds(compiler, work, problems)
        qualified_literals(compiler, work, problems)
        impl_statics(compiler, work, problems)
        impl_order(compiler, work, problems)
        interface_scope(compiler, work, problems)
        driver_messages(compiler, work, problems)
        header_include_dirs(compiler, work, problems)
        # The interface records' cache (ADR 0082) reads fstat's layout, known for Linux and macOS only: on FreeBSD
        # no record is made and every check is the full one (ADR 0149 §2.3), so there is nothing to check.
        freebsd = sys.platform.startswith("freebsd")
        if not freebsd:
            interface_records(compiler, work, problems)
        check_all_rounds(compiler, work, problems)
        windowed_executables(compiler, work, problems)
        manifest_targets(compiler, work, problems)
        git_subdirectories(compiler, work, problems)
        dependency_patch(compiler, work, problems)
        manifest_frameworks(compiler, work, problems)
        library_build_default(compiler, work, problems)
        polls_from_manifest(compiler, work, problems)
        file_private_names(compiler, work, problems)

    skipped = " (interface records skipped: FreeBSD keeps none)" if freebsd else ""
    print(f"package_tests: 42 checks; {len(problems)} problems{skipped}")
    for problem in problems:
        print("  FAIL " + problem)
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
