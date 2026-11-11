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
    - evilroots, a git dependency with roots outside its checkout, gets N0362; so does `check` run inside its checkout
      and the language server opening its file there (a package in the package cache embeds only from itself, even as
      the root package). evilout's path dependency lies outside the package cache (made here) and has roots outside
      it: N0362 through the victim, and with `check` run in evilout's checkout (its lock committed for that).
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

usage: python3 tools/package_tests.py --compiler out/ceesafec
"""

import argparse
import itertools
import json
import os
import random
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


def language_server(compiler, path, hover=None):
    """What `ceesafec lsp` answers for the file at `path` once opened: the diagnostics it publishes, as JSON text, and
    with `hover` (a line and a character) the hover's answer; both empty on a timeout (a FIFO that hung the check
    would be one)."""
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
        messages.append({"jsonrpc": "2.0", "id": 7, "method": "textDocument/hover", "params": {
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
    if os.name != "nt":
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

        interface_records(compiler, work, problems)
        check_all_rounds(compiler, work, problems)

    print(f"package_tests: 30 checks; {len(problems)} problems")
    for problem in problems:
        print("  FAIL " + problem)
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
