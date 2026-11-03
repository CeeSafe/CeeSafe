# CeeSafe in GitHub Linguist (draft)

GitHub names a repository's languages, and highlights code, with [Linguist](https://github.com/github-linguist/linguist).
It does not know CeeSafe, so `.cee` files count for nothing in a repository's language bar and are not highlighted.
This directory is the pull request that would add CeeSafe, ready for when Linguist will accept it.

## Status (2026-11-03): not yet submittable

Linguist's CONTRIBUTING.md, "Language extension and filename usage requirements":

- at least **2000 files** per extension indexed by GitHub search in the last year, excluding forks;
- "a reasonable distribution across unique `:user/:repo` combinations"; the language owner's own repositories are
  filtered out with `-user:`;
- "we do not accept PRs for very new or hobby languages, and will close any such PRs".

On 2026-11-03 GitHub's code search counted 1084 `.cee` files. Of those, 130 were outside `vkuttyp` and the CeeSafe
organisation, and none was CeeSafe: they belong to other languages that use `.cee` (`Leprekus/ceelish-compiler`,
`mohamettapthi75-dot/CeejiyeLang`, `cptx032/cee`, `RandomProgrammerOnTheInternet/cee-lang`) or to coursework.
`python3 tools/linguist_usage.py` measures this again.

## What is here

| File | Goes to (in github-linguist/linguist) |
|---|---|
| `languages.yml` | the entry in `lib/linguist/languages.yml`; `script/update-ids` adds its `language_id` |
| `heuristics.yml` | `lib/linguist/heuristics.yml`, only if another language claims `.cee` by then |
| `samples/CeeSafe/*.cee` | `samples/CeeSafe/` |

- **Samples**, real CeeSafe code, refreshed from their sources when submitting:
  - `channel.cee` is `lib/std/channel.cee`, GPL-2.0-only WITH Classpath-exception-2.0 (its SPDX header);
  - `reactor.cee` is `benchmarks/async-reactor/reactor.cee`, GPL-2.0-only (the repository's licence, `COPYING`).
- **The heuristic** matches both samples, and none of the other languages' files checked
  (`CeejiyeLang/examples/loops.cee`, `cptx032/cee/examples/defer/main.cee`, `ceelish-compiler/tests/factorial.cee`).
- **The colour**, `#128d99`, is the teal of the extension's icon. Linguist's tests refuse a colour too close to
  another language's; choose another if they do.

## The grammar

Linguist highlights with TextMate grammars under a licence on its list (`vendor/licenses/config.yml`: MIT, Apache-2.0,
BSD, ISC, MPL-2.0, zlib and a few others). CeeSafe's grammar, `editors/vscode/syntaxes/ceesafe.tmLanguage.json` (scope
`source.ceesafe`), is under the MIT licence (`editors/vscode/syntaxes/LICENSE`; the owner's choice, 2026-11-03), while
the rest of the extension stays GPL-2.0-only. Linguist adds a grammar from its own repository
(`script/add-grammar <url>`), so before submitting, publish it as one, for example `CeeSafe/ceesafe-grammar`, with the
grammar and that licence.

## Submitting

When `tools/linguist_usage.py` shows the threshold met by others' code:

1. Fork github-linguist/linguist and set up its environment (its CONTRIBUTING.md: Ruby, Docker for grammars).
2. Add the `languages.yml` entry, then `script/add-grammar https://github.com/CeeSafe/ceesafe-grammar`.
3. Copy the samples (fresh from their sources) into `samples/CeeSafe/`, and the heuristic if `.cee` is shared.
4. `script/update-ids`, then `bundle exec rake test` and `bundle exec script/cross-validation --test`.
5. Open the pull request with the search link the template asks for (`extension:cee` excluding the owner's accounts)
   and the samples' licences.

GitHub picks up a merged change with Linguist's next release, deployed to github.com some weeks later.
