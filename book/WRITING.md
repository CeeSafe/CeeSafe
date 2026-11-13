# Writing the book

How a chapter of *The CeeSafe Programming Language* is written (Decision 0011, `docs/decisions/0011-the-ceesafe-book.md`).
Read this, `book/README.md`, and two finished sections as the pattern before writing: `src/ch01-02-hello-world.md`
(a first section) and `src/ch04-02-borrowing.md` (a section with a rule that the compiler enforces).

## The readers

A person who can program in some language and knows no CeeSafe, Rust or C; and an AI model that will read one
section on its own. So:

- **A section stands alone.** It says in its first paragraph what it teaches and which sections it assumes, with
  links. It never says "as we saw".
- **Teach only with what earlier chapters taught.** A chapter may use a later feature only if it says in a sentence
  what the feature does and names the chapter that teaches it ("chapter 9 covers errors"). Do not link to a chapter
  that is not written: name it in plain text.
- **One term for one thing.** "Borrow" (not "reference" or "pointer"), "package", "listing", "diagnostic", "the
  compiler" or `ceesafec`.
- **Plain prose.** Short sentences, present tense, "you". No jokes that need culture, no "simply" or "just", no
  marketing. Say what a thing does before why.

## What is true

CeeSafe is new, and what you remember from Rust, C#, Go or TypeScript is often wrong for it. **Nothing goes in the
book that you have not seen the compiler do.**

- The compiler is `out/ceesafec` in this repository (run it from the repository root; `ceesafec` may not be on the
  `PATH`, and if it is, it may be older).
- Sources of truth, in order: the compiler's behaviour; `spec/` (the rules; `spec/type-system.md` is the main one);
  `guide/` (the same topics as reference pages, with checked examples: good material, not text to copy);
  `guide/builtins.md` and `spec/api/core.md`, `spec/api/std.md` for names; `AGENTS.md` for the mistakes people make.
- Look a name up before using it. If a function you want does not exist, do not invent it and do not work around
  it silently: use what exists, and report the gap at the end.
- Every output and every diagnostic in the text is pasted from a real run, never typed from memory.

## A page

One file for each section, `src/<id>.md`, where the id is given to you (`ch03-02-data-types`) and never changes.
A chapter also has an opening page (`ch03-00-…`): a few paragraphs on what the chapter covers and why, with a
linked list of its sections, and no listings unless one helps.

```markdown
---
title: Data types
summary: One sentence, 15 to 40 words, saying what the section teaches, for a table of contents a model reads. No colon in it.
assumes: [ch03-01-variables]
keywords: [i32, u8, f64, bool, array]
diagnostics: [N0205]
specification: ["type-system.md §2"]
---

# Data types

**This section teaches** … It assumes [Variables](./ch03-01-variables).
```

- `summary` must not contain a colon followed by a space (the header is read by a simple parser). Use a dash.
- `diagnostics` lists the codes this page shows in a listing, and only those. Appendix F's "Shown in" column is
  written from these lists: after a change to one, run `python3 tools/gen_appendix_f.py` and commit the page. (A
  section that starts or stops naming a built-in type's member can change Appendix C's "Taught in" column the same
  way: `python3 tools/gen_appendix_c.py`. `tools/book_check.py` says when either is needed.)
- `specification` only if you have opened the file and found the section.

Then the parts, in this order:

1. **The idea,** in prose, with the first listing early. `##` headings for the parts of the idea.
2. **Listings** as they are needed (below).
3. **Rules,** each in a box where it is taught: `::: tip Rule` … `:::`. One or two sentences. Add
   `*Specification: …*` as the box's last line when you have the reference.
4. **`## Mistakes`:** each a box `::: warning Mistake: what it is` with a sentence, then a listing that does not
   compile (or misbehaves) with the compiler's real message, then the fix in a sentence or a listing. Choose the
   mistakes newcomers and models really make; `AGENTS.md` has a list.
5. **Coming from … notes,** after the mistakes, where a habit from another language misleads:
   `::: details Coming from Rust` … `:::`. Only where there is something real to say.
6. **`## Summary`:** the rules again as a list, then one line on what comes next.

A section is 600 to 1,500 words, not counting listings. If it needs more, tell the editor: it is two sections.

## Listings

A listing is a **whole program** in one `ceesafe` block: it has a `fn main()` unless it is there to show something
that needs none. Keep them short: the smallest program that shows the point, 5 to 30 lines.

````markdown
<span class="listing-caption"><strong>Listing 3-4</strong>, <code>main.cee</code>: What the listing shows, as a sentence.</span>

```ceesafe
fn main() {
    Console.writeLine(6 * 7);
}
```

```text
42
```
````

- Number them through the chapter: `Listing <chapter>-<n>`. Your first number is given to you if the chapter
  already has listings.
- The caption line is exactly that HTML, then a blank line, then the block. For a listing that must not compile or
  only warns, a comment goes directly before the block, after the blank line:
  `<!-- ceesafe: fail N0240 -->`, `<!-- ceesafe: warns N0400 -->`, `<!-- ceesafe: panics -->`,
  `<!-- ceesafe: exits 3 -->`, `<!-- ceesafe: check -->` (type-checked, not run: it reads input or calls C).
- The `text` block right after is the **exact** output, or the exact compiler message. The checker compares it.
- A program is run with no arguments and empty standard input. A program that reads input must behave sensibly at
  end of input, or use `<!-- ceesafe: check -->` and show a sample session in prose or in a `sh`-style block that
  is not directly after the listing.
- You may repeat part of a listing in a small unnumbered `ceesafe` block to talk about it, but that block is also
  compiled as a whole program, so it must be one. Prefer quoting a line in backticks.
- Run `out/ceesafec fmt` on every listing: the book shows the one style.

**Checking.** `tools/guide_check.py` compiles and runs every block and compares the `text` blocks. Check only your
own pages: copy them to an empty directory and run

```sh
python3 tools/guide_check.py --ceesafec out/ceesafec --guide <that directory>
```

from the repository root. It must report 0 problems. Read the script's header for what each comment does.

**Programs of several files** (packages, tests, the projects) cannot be one block. Make the real package under
`book/projects/<name>/` (with its `ceesafe.toml`), check it there with `out/ceesafec check`, `run --interp`,
`test`, and show its files in the text as listings whose caption names the path
(`<code>src/config.cee</code>`), with `<!-- ceesafe: skip -->` before the block if the file cannot compile on its
own. The text must match the file exactly; say in your report which listings are copies of which files.

## Markdown that the site accepts

The site compiles pages as Vue, and fails on dead links.

- Outside code, never write `<` followed by a letter, or `{{`. Put generic types in backticks: `` `Vector<u8>` ``.
- Links to other pages of the book are relative, with the page's id and no extension: `[Variables](./ch03-01-variables)`,
  `[the rule](./ch04-02-borrowing#the-rule)`. Link only to ids on the list you were given.
- Links into the documentation site are full URLs: `https://ceesafe.github.io/CeeSafe/guide/errors`.
- Boxes: `::: tip Rule`, `::: warning Mistake: …`, `::: details Coming from …`, `::: info …` for a note.
- Tables are fine. No images.

## Do not

- Do not edit `toc.mjs`, the site's configuration, or another chapter's pages. Report what the table of contents
  should say if your sections differ from the plan.
- Do not change the compiler, the specification or the guide. If the language lacks something the chapter needs,
  or a diagnostic is misleading, or the guide is wrong, say so in your report.
- Do not commit.

## The report

When you finish, report: the files you wrote; the number of listings and the checker's last line; every place the
plan and the language disagreed and what you did; every gap in the language or library you met; anything in
`guide/` or `spec/` you found to be wrong.
