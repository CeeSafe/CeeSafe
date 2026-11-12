# The book

The source of *The CeeSafe Programming Language* (Decision 0011): a book in the shape of the Rust Book, for people
and for AI models, beside the documentation site (`site/`). **A prototype:** chapter 1 and section 4.2 are written.

| Path | What |
|---|---|
| `toc.mjs` | the table of contents: every chapter and section, written or planned |
| `src/*.md` | one file for each written page; its name is the page's stable identifier |
| `src/public/` | **generated**, not committed: `llms.txt`, `llms-full.txt`, `index.json`, each page's Markdown, each listing as a file |
| `scripts/prepare.mjs` | writes `src/public/` |
| `.vitepress/` | the site: the numbered sidebar from `toc.mjs`, the theme |

## Working on it

```sh
cd book
vp install            # or: pnpm install
vp run dev            # a local server with live reload
vp run build          # fails on a dead link, or a page without its header
```

Every `ceesafe` block is a whole program, checked as the guide's are:

```sh
python3 tools/guide_check.py --ceesafec out/ceesafec --guide book/src
```

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
