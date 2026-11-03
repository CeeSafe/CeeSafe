# The documentation site

The source of [ceesafe.github.io/CeeSafe](https://ceesafe.github.io/CeeSafe/), built with
[VitePress](https://vitepress.dev/) and published to GitHub Pages by `.github/workflows/docs.yml` on every push to
`main` that changes the site, `spec/`, `guide/` or the grammar.

| Path | What |
|---|---|
| `index.md` | the home page |
| `guide/`, `reference/` | **generated**, not committed: `scripts/prepare.mjs` copies the repository's `guide/` and `spec/` here at build time, rewriting links to other files of the repository into GitHub URLs and escaping text Vue would misread, and writes the decisions' index |
| `public/llms.txt`, `public/llms-full.txt`, `public/guide/`, `public/reference/` | **generated**, not committed: the [llms.txt](https://llmstxt.org) index for AI agents, the guide and specification in one file, and each page's Markdown at its URL with `.md` |
| `.vitepress/config.mts` | navigation, sidebars, search, and CeeSafe highlighting from `editors/vscode/syntaxes/ceesafe.tmLanguage.json` |

## Working on it

```sh
cd site
pnpm install          # or: vp install
pnpm run dev          # a local server with live reload
pnpm run build        # what CI runs; fails on a dead link
```

Edit the guide in `guide/` and the specification in `spec/`, not their copies here. `ceesafec` embeds the same files,
so after changing them rerun `tools/gen_docs.py` and `tools/gen_catalog.py` and rebuild `ceesafec`
(`tools/spec_check.py` checks that). Every `ceesafe` example of the guide is compiled by `tools/guide_check.py`
(`spec/testing.md` §4).
