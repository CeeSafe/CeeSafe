# Editors

## VS Code

The extension in `editors/vscode` gives `.cee` files syntax highlighting, and through the language server: errors as
you type, the type or signature on hover, go to definition (F12), the outline, and formatting (Shift+Alt+F).

Install it from the [Visual Studio Marketplace](https://marketplace.visualstudio.com/items?itemName=ceesafe.ceesafe-language):
search for **CeeSafe** in VS Code's Extensions view, or run

```sh
code --install-extension ceesafe.ceesafe-language
```

Every [release](https://github.com/CeeSafe/CeeSafe/releases) also has it as `ceesafe-language-<version>.vsix`, built and
tested with that release's `ceesafec` (its checksum is in `SHA256SUMS`), for **Extensions: Install from VSIX…**.

To build it from the repository instead, with [Vite+](https://viteplus.dev/):

```sh
cd editors/vscode
vp install
vp test                 # the grammar, and ceesafec lsp through VS Code's own client library
vp run package          # makes ceesafe-language-<version>.vsix
code --install-extension ceesafe-language-*.vsix
```

The extension starts `ceesafec lsp`. It finds `ceesafec` on your `PATH`, or where the setting `ceesafe.server.path` says.

## Other editors

`ceesafec lsp` is a standard Language Server Protocol server on standard input and output, so any editor with an LSP
client can use it: Neovim, Helix, Emacs, Zed, Sublime Text. Configure it to run `ceesafec lsp` for `.cee` files. The
TextMate grammar in `editors/vscode/syntaxes/ceesafe.tmLanguage.json` works in editors that accept TextMate grammars.

## What the server does

| | |
|---|---|
| Diagnostics | every error and note of `ceesafec check`, as you type |
| Hover | the type of an expression; `let`/`var` and the type of a variable; a function's signature; a field's type |
| Go to definition | locals, parameters, functions, methods, fields, structs, enums and interfaces in the file |
| Document symbols | the outline: functions, structs with their fields and methods, enums, interfaces, constants |
| Formatting | the whole file, in `ceesafec fmt`'s style |

A file that belongs to a package is checked with the package and its locked dependencies; any other file is checked
on its own. The details are in [Tooling §2](../spec/tooling.md#_2-language-server-t3).
