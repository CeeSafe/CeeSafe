# Packages

A package is a directory with a `ceesafe.toml`: a program or a library, its sources, and the packages it depends on.
Dependencies come from local paths or git repositories, pinned to exact commits in `ceesafe.lock`. The full rules are in
[Tooling §4](../spec/tooling.md#_4-packages-t6-adr-0013) and [ADR 0013](../spec/decisions/0013-m10-modules-and-packages.md).

## The manifest

```toml
[package]
name = "app"                 # an identifier: what other packages call this one
version = "0.1.0"
kind = "executable"          # "executable", "library", or "object" (one .o for a kernel's build)
profile = "hosted"           # or "freestanding"
float = true                 # false is --no-float
sources = ["src"]            # the default
c = ["src/shim.c"]           # C files built with an executable

[dependencies]
geometry = { path = "../geometry" }
lz4 = { git = "https://example.org/lz4-cee.git", tag = "v1.2.0" }      # or rev = "<commit>", or branch = "main"
```

Only `name` is required. A dependency's key must be the name its own manifest gives it, and a dependency must be a
library.

## Using a dependency

All files of one package share one scope. Another package's items are reached through its name, and only its
`public` items are visible:

<!-- ceesafe: skip -->
```ceesafe
// geometry/src/lib.cee
public struct Point {
    public x: i64;
    public y: i64;

    public fn create(x: i64, y: i64) -> Point => Point { x: x, y: y };
    public fn norm(this) -> i64 => this.x * this.x + this.y * this.y;
}

public fn origin() -> Point => Point.create(0, 0);

fn helper() -> i64 => 1;          // internal: only geometry can call it
```

<!-- ceesafe: skip -->
```ceesafe
// app/src/main.cee
import geometry.Point;            // one item; `import geometry;` brings all its public items

fn main() {
    let p = Point.create(3, 4);   // imported
    let q = geometry.origin();    // qualified
    Console.writeLine(p.norm() + q.norm());
}
```

- `geometry.X` names a dependency's public item, in types, expressions and patterns.
- `import geometry.X;` brings one item into the file's scope, and `import geometry;` all its public items. Imports
  apply to their file only. A name two imports provide is an error where it is used.
- Within a package, the name of a dependency is reserved: nothing else in the package may be called `geometry`.
- A public function may not expose a type its users cannot see (a public function returning an internal struct).
- Two packages may declare the same name: in the generated C, a dependency's items carry its package's name.

## The lock file

```sh
ceesafec fetch          # resolves dependencies and writes ceesafe.lock
ceesafec build          # uses ceesafe.lock; fails if it does not match ceesafe.toml
```

`ceesafe.lock` records every dependency, direct and indirect: where it comes from, and for a git dependency the exact
commit and a SHA-256 of its files. Commit it with `ceesafe.toml`, so that every build of the package uses the same code.

- A `tag` or `branch` is resolved to a commit by `ceesafec fetch` and stays that commit until the next fetch, even if the
  branch moves.
- Every build checks each git dependency's files against the lock's hash, so a checkout changed behind your back is
  an error.
- Git repositories and checkouts are cached in `~/.ceesafe` (or `$CEESAFE_HOME`). `--offline` builds never touch the network.
- Nothing runs during a fetch or a build except `git` and the C compiler: packages have no build scripts.
- Two path or git dependencies that need the same package must agree on its source and commit.

## The registry

A dependency can also be a version requirement, looked up in the package index:

```toml
[dependencies]
shapes = "2.1"                # any 2.x from 2.1.0 on
geometry = "=1.4.0"           # exactly
json = { version = "0.9", index = "https://example.org/my-index.git" }
```

`ceesafec fetch` chooses the newest version of each package that satisfies every requirement on it, and records its
commit and hash in `ceesafe.lock`, so builds stay pinned until the next fetch. A version already locked is kept while it
still fits; `ceesafec fetch --update` (or `--update shapes`) chooses afresh. When no choice works, the error says which
requirements clash and where each came from.

To publish a library, give its manifest a `version`, commit and push it, and run `ceesafec publish`. It checks the package
and prints the entry to add to the index by pull request.

## Building

```sh
ceesafec check          # the package and its dependencies, checked together
ceesafec build          # out/<name>
ceesafec run --interp
ceesafec fmt            # the package's own sources
ceesafec doc -o api.md  # its public items
```

A build compiles the package and all its dependencies together into one C program, so generic code from a dependency
is specialised for its users and checked like the rest.

## Files without a package

Everything also works on files: `ceesafec build a.cee b.cee -o tool`. Files given together are one package with no
dependencies.
