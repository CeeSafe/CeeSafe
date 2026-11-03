# CeeSafe for VS Code

Language support for [CeeSafe](https://github.com/CeeSafe/CeeSafe), the memory-safe systems programming language that
compiles to C: syntax highlighting for `.cee` files, and, through the `ceesafec lsp` language server,

- errors as you type, with their diagnostic codes;
- the type of an expression, a variable or a field, and a function's signature, on hover;
- go to definition (F12) for locals, parameters, functions, methods, fields and types;
- the outline of a file;
- formatting (Shift+Alt+F) in `ceesafec fmt`'s one canonical style.

A file that belongs to a package (a `ceesafe.toml`) is checked with the package and its locked dependencies.

## Requirements

The extension starts `ceesafec`, the CeeSafe compiler. Install it on macOS or Linux with

```sh
curl -fsSL https://ceesafe.github.io/CeeSafe/install.sh | sh
```

or on Windows, in PowerShell, with `irm https://ceesafe.github.io/CeeSafe/install.ps1 | iex`
([installation](https://ceesafe.github.io/CeeSafe/guide/installation)).

## Settings

| Setting               | Default    |                                                    |
| --------------------- | ---------- | -------------------------------------------------- |
| `ceesafe.server.path` | `ceesafec` | the `ceesafec` to run, if it is not on your `PATH` |

## Links

- [The CeeSafe guide](https://ceesafe.github.io/CeeSafe/guide/introduction) and [editors](https://ceesafe.github.io/CeeSafe/guide/editors)
- [Releases](https://github.com/CeeSafe/CeeSafe/releases): `ceesafec` for six platforms, and this extension as a `.vsix`
- [Issues](https://github.com/CeeSafe/CeeSafe/issues)

## Building from the repository

With [Vite+](https://viteplus.dev/) (`vp`), in `editors/vscode`:

```sh
vp install
vp test                 # grammar tests, and ceesafec lsp through vscode-jsonrpc
vp run package          # makes ceesafe-language-<version>.vsix
code --install-extension ceesafe-language-*.vsix
```

## License

GPL-2.0-only, as CeeSafe, except the TextMate grammar, `syntaxes/ceesafe.tmLanguage.json`, which is under the MIT
licence (`syntaxes/LICENSE`) so that other tools, GitHub's Linguist among them, can use it (`editors/linguist/`).
