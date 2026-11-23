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
| `src/ceesafe.rs` | starts `ceesafec lsp` (built to `wasm32-wasip1` against `zed_extension_api` 0.7) |
| `languages/ceesafe/config.toml` | `.cee`, comments, brackets |
| `languages/ceesafe/*.scm` | the queries: highlights, brackets, indents, outline |
| `../tree-sitter-ceesafe` | the grammar: `grammar.js`, the generated `src/parser.c`, and `src/scanner.c` |

## Checking

```sh
cargo build --release --target wasm32-wasip1                    # the extension
cd ../tree-sitter-ceesafe && tree-sitter generate                # the parser, after a change to grammar.js
python3 tools/tree_sitter_check.py --tree-sitter "$(which tree-sitter)" --highlights   # from the repository root
```

The last parses every CeeSafe source of the compiler, the libraries and the corpus, and compiles the queries.

## Publishing

Not done yet. Zed's extension registry lists an extension from the
[zed-industries/extensions](https://github.com/zed-industries/extensions) repository, with its licence; whether
CeeSafe's (GPL-2.0-only with the Classpath exception) is one it accepts is to be checked.
