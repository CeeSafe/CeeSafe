# The book

The source of *The CeeSafe Programming Language* (Decision 0011): a book in the shape of the Rust Book, for people
and for AI models, beside the documentation site (`site/`). Chapters 1 to 22 are written (1 to 12 are
the course); the appendices and the glossary are planned. It is published with the documentation site, at
<https://ceesafe.github.io/CeeSafe/book/> (`.github/workflows/docs.yml`).

| Path | What |
|---|---|
| `toc.mjs` | the table of contents: every chapter and section, written or planned |
| `src/*.md` | one file for each written page; its name is the page's stable identifier |
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

How a chapter is written: `WRITING.md`.

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
