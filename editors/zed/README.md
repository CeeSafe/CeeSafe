# CeeSafe for Zed

Language support for [CeeSafe](https://github.com/CeeSafe/CeeSafe) in [Zed](https://zed.dev): syntax highlighting,
the outline, bracket matching and indentation from a Tree-sitter grammar, and, through the `ceesafec lsp` language
server, errors as you type, hover, go to definition, formatting and quick fixes.

## Requirements

`ceesafec`, the CeeSafe compiler, on your `PATH`
([installation](https://ceesafe.github.io/CeeSafe/guide/installation)), or its path in Zed's settings:

```json
{ "lsp": { "ceesafec": { "binary": { "path": "/opt/ceesafe/bin/ceesafec" } } } }
```

## Installing from the repository

In Zed, run **zed: install dev extension** from the command palette and choose this directory. Zed builds the
extension with Rust (installed with `rustup`) and the grammar from the commit `extension.toml` names.

Before the grammar's commit is on GitHub, point the grammar at your checkout for the dev install: in
`extension.toml`, set `repository = "file:///path/to/CeeSafe"` and `rev` to a local commit that has
`editors/tree-sitter-ceesafe`.

## Layout

| | |
|---|---|
| `extension.toml` | the manifest: the grammar (repository, commit, path) and the language server `ceesafec` |
| `src/ceesafe.rs` | starts `ceesafec lsp` (built to `wasm32-wasip2` against `zed_extension_api` 0.7) |
| `languages/ceesafe/config.toml` | `.cee`, comments, brackets |
| `languages/ceesafe/*.scm` | the queries: highlights, brackets, indents, outline |
| `LICENSE` | MIT, this directory's |
| `../tree-sitter-ceesafe` | the grammar: `grammar.js`, the generated `src/parser.c`, and `src/scanner.c` |

## Checking

```sh
cargo build --release --target wasm32-wasip2                    # the extension
cd ../tree-sitter-ceesafe && tree-sitter generate                # the parser, after a change to grammar.js
python3 tools/tree_sitter_check.py --tree-sitter "$(which tree-sitter)" --highlights   # from the repository root
```

The last parses every CeeSafe source of the compiler, the libraries and the corpus, and compiles the queries.

## Publishing

Zed's registry lists an extension from the [zed-industries/extensions](https://github.com/zed-industries/extensions)
repository: a pull request adds it as a submodule (this repository, `path = "editors/zed"`) with its version. The
first is opened by hand, after the extension has been tried in Zed at the commit it names; after that, the release
workflow opens one for each release (docs/release.md).

## Licence

This directory, the extension's own code, is under the MIT licence (`LICENSE`), one of those Zed's registry accepts.
The rest of CeeSafe, the compiler that the extension starts and the grammar it names among it, keeps its licence
(`COPYING`, `COPYING.RUNTIME`).
