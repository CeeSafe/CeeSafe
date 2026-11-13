# The book

The source of *The CeeSafe Programming Language* (Decision 0011): a book in the shape of the Rust Book, for people
and for AI models, beside the documentation site (`site/`). Chapters 1 to 22 (1 to 12 are the course),
Appendices A to J and the glossary are written. It is published with the documentation site, at
<https://ceesafe.github.io/CeeSafe/book/> (`.github/workflows/docs.yml`).

| Path | What |
|---|---|
| `toc.mjs` | the table of contents: every chapter and section, written or planned |
| `src/*.md` | one file for each written page; its name is the page's stable identifier |
| `appendices/` | the hand-written inputs of the generated appendices (not pages): `appendix-f.intro.md` and `appendix-f.lines.txt`, `appendix-h.template.md`, `appendix-c.api.txt` |
| `../tools/gen_appendix_c.py`, `gen_appendix_f.py`, `gen_appendix_h.py` | the generators of Appendices C, F and H (below); `../tools/book_appendix_gen.py` is what they share |
| `projects/` | the packages the chapters show: programs of several files, tests, the projects, and variants that are there to fail (`.book-expect`) |
| `src/public/` | **generated**, not committed: `llms.txt`, `llms-full.txt`, `index.json`, each page's Markdown, each listing as a file |
| `scripts/prepare.mjs` | writes `src/public/` |
| `version.mjs` | the compiler's version, read from `src/backend/c_program.cee`: a page writes `%ceesafec-version%` |
| `.vitepress/` | the site: the numbered sidebar from `toc.mjs`, the theme |

## Working on it

```sh
cd book
vp install            # or: pnpm install
vp run dev            # a local server with live reload
vp run build          # fails on a dead link, or a page without its header
```

Every `ceesafe` block is a whole program, checked as the guide's are, and the pages and the packages under
`projects/` have their own check (it is one of the contract tests):

```sh
python3 tools/book_check.py --ceesafec out/ceesafec
```

`.github/workflows/checks.yml` builds ceesafec from the tree and runs it, with the guide's and the specification's
checks, on every pull request and every push to `main` that touches the book, the documents or the compiler: the
site build alone (`docs.yml`) does not compile a listing.

How a chapter is written: `WRITING.md`.

## Regenerating the appendices

Three appendices are generated, and say so in a comment under their title. Never edit those pages: change the
input, then run the generator from the repository's root and commit the page with it.

| Page | Command | Written from |
|---|---|---|
| `src/appendix-c-built-in-types.md` | `python3 tools/gen_appendix_c.py` | the text and the rows in the script itself; the "Taught in" column from the chapters that name each member |
| `src/appendix-f-diagnostics.md` | `python3 tools/gen_appendix_f.py` | `appendices/appendix-f.intro.md` (the opening), `appendices/appendix-f.lines.txt` (a line for each code), the codes of `src/diagnostics/catalog.cee`, the chapters' `diagnostics:` headers; the groups are in the script |
| `src/appendix-h-grammar.md` | `python3 tools/gen_appendix_h.py` | `appendices/appendix-h.template.md` (the text, with `@@NAME@@` where productions go), `spec/grammar.md`, `spec/type-system.md` §15.2 |

- A generator takes no argument and runs no compiler. With `--check` it writes nothing and exits 1, with the lines
  that differ, when the committed page is not what it would write.
- `tools/book_check.py` makes the same comparison, so a page that is stale fails the contract suite with the
  command to run. A page goes stale when the grammar changes (H), when the catalog gets a code or a chapter's
  header names another diagnostic (F), and when a chapter starts or stops naming a member (C).
- A new diagnostic code needs its line in `appendix-f.lines.txt` first: the generator says which code has none.
- Appendix C's rows give a member's signature as a program writes the call, so they are not copied from the API
  pages. `appendices/appendix-c.api.txt` records the declarations of `spec/api/` the rows were written from; when
  one changes, the generator names it and writes nothing. Correct the member's row in `tools/gen_appendix_c.py`,
  then run `python3 tools/gen_appendix_c.py --api` to record the declarations, and the generator again.

## A page

- A header: `title`, `summary` (one sentence, for `llms.txt`), and where they apply `assumes`, `keywords`,
  `diagnostics` and `specification`. A diagnostic named there must be one a listing of the page shows.
- Its parts in order: what it teaches and assumes; the idea; listings; rules; mistakes; "coming from" notes; a
  summary.
- A listing is a caption line, then the block, then what it prints or the compiler says:

  ````markdown
  <span class="listing-caption"><strong>Listing 1-1</strong>, <code>main.cee</code>: A program that prints a line.</span>

  ```ceesafe
  fn main() {
      Console.writeLine("Hello, world!");
  }
  ```

  ```text
  Hello, world!
  ```
  ````

- A rule is `::: tip Rule`, a mistake `::: warning Mistake: …`, a note for readers of another language
  `::: details Coming from …`.
