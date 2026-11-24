# Decision 0012 — The licences of the editor tooling

- Status: **Proposed**, 2026-11-24, under the optimization and DX roadmap's Phase 4, item 2 (the owner: "Continue to
  the remaining in the roadmap"). Nothing is relicensed by this document: §4's questions are the owner's, and only the
  copyright holders can answer the second.
- Relates to: `COPYING`, `COPYING.RUNTIME`, tooling.md §2.4 and §2.5, `editors/linguist/README.md`.

## 1. What the repository says today

| Part | What its files say | Where |
|---|---|---|
| The compiler, tools, tests, spec, docs | GPL-2.0-only | `COPYING`; `COPYING.RUNTIME`'s last paragraph |
| The runtime, `lib/core`, generated C | GPL-2.0-only WITH Classpath-exception-2.0 | `COPYING.RUNTIME` |
| The language server (`src/tooling/language_server.cee`) | GPL-2.0-only; compiled into `ceesafec` | `COPYING` |
| The VS Code extension's client (`editors/vscode`, but its grammar) | GPL-2.0-only | `package.json` `"license"` |
| The TextMate grammar (`editors/vscode/syntaxes/ceesafe.tmLanguage.json`) | **MIT**, the owner's choice of 2026-11-03, so GitHub Linguist can use it | `editors/vscode/syntaxes/LICENSE`, `editors/linguist/README.md` |
| The Zed extension (`editors/zed`) | **MIT**, as Zed's registry requires of an extension's code | its `LICENSE`, `Cargo.toml`, SPDX lines, tooling.md §2.5 |
| The Tree-sitter grammar (`editors/tree-sitter-ceesafe`) | GPL-2.0-only WITH Classpath-exception-2.0 | SPDX lines, `tree-sitter.json` |
| The debugger scripts (`editors/debug`) | no SPDX line: GPL-2.0-only by `COPYING.RUNTIME` | |

## 2. What does not agree

1. `COPYING.RUNTIME` says that everything but the runtime, `lib/core` and generated C, "the editor support"
   included, is GPL-2.0-only without the exception. Two parts of the editor support are MIT already, by the owner's
   choice (§1), and the sentence does not say so.
2. The Tree-sitter grammar carries the runtime's exception, which `COPYING.RUNTIME` grants to the files it lists
   ("this library"), and the grammar is not one of them. Its SPDX lines claim a grant the notice does not make.
3. The grammar is compiled into the Zed extension, which is MIT; other editors (Helix, Neovim's Tree-sitter plugins)
   bundle grammars into their own builds as well. A grammar under the GPL makes each such build a combined work.

## 3. The facts a choice turns on

- **The language server is inside `ceesafec`.** Putting `language_server.cee` under MIT would not change the licence of
  the binary, which is GPL-2.0-only as a whole; the roadmap's item says so too. Only a separate work, one that is
  distributed alone, changes anything by its own licence.
- **Separate works:** the VS Code extension (a `.vsix` on its own), the TextMate grammar, the Tree-sitter grammar and
  the Zed extension. Each talks to `ceesafec lsp` over a protocol, not by linking.
- **Who holds the copyright.** This repository's history has one author. The Tree-sitter grammar and the Zed extension
  were written here on 2026-11-23. `editors/vscode` and `editors/debug` were added on 2026-10-31 "under CeeSafe names"
  (commit `421f9013`): brought from the language CeeSafe forked from (`COPYING.RUNTIME`), so whoever holds the
  copyright in that code has to agree before they leave the GPL.

## 4. Questions for the owner

1. **The Tree-sitter grammar: MIT**, as the TextMate grammar is and for the same reason (other editors bundle it), with
   its `LICENSE`, SPDX lines and `tree-sitter.json` changed? **Proposed: yes.**
2. **The VS Code extension's client** (`extension.js`, `package.json`, `language-configuration.json`): MIT, or GPL-2.0-only
   as now? It is about 30 lines that start `ceesafec lsp`, and nothing requires MIT of it (the VS Code marketplace takes
   GPL extensions). If MIT, the holder of the forked-from code's copyright has to agree (§3). **Proposed: leave it
   GPL-2.0-only** until a reason appears.
3. **The language server and the debugger scripts:** stay GPL-2.0-only? **Proposed: yes** (§3: relicensing the
   server's file changes nothing about the binary).
4. **`COPYING.RUNTIME`'s last paragraph** then names the MIT parts: "…the editor support, except the TextMate grammar,
   the Tree-sitter grammar and the Zed extension, which are MIT (their `LICENSE` files)…". **Proposed: yes**, whatever
   the answers to 1 and 2, since two parts are MIT today.

This is not legal advice: the questions are about what the repository should say, and the owner may want it read by
someone who gives it.
