# Decision 0011 — The CeeSafe book

- Status: **Accepted** by the owner, 2026-11-12, on the prototype ("Yes. Really good design. I like that."). The
  tool is VitePress, run through Vite+ as the site is. Still open: the authors' line and the licence of the text
  (§8). Asked for by the owner: "We need Rust's book style that will help new users to
  learn CeeSafe in detail and it will be a reference to the language … a comprehensive guide for the language for both
  human and AI." The documentation site stays as it is.
- Relates to: Decision 0006 (the specification, and the guide's examples being checked), the guide (`guide/`), the
  site (`site/`), `guide/for-agents.md` and the MCP server (`spec/tooling.md` §6).
- Built so far, as a prototype (`book/`): the site, the machine-readable files of §5, chapter 1 and section 4.2.
  Not yet: `tools/book_check.py`, the print page, the version written by the build, publishing.

## 1. Context

CeeSafe has two kinds of documentation:

- **The guide** (`guide/`, 26 pages, about 33,000 words): one page for each topic, each standing alone. It suits a
  reader who knows what to look up. Its examples are compiled and run by `tools/guide_check.py`.
- **The specification** (`spec/`): the rules, normative, written for the compiler's authors.

Neither teaches the language from the start. A newcomer has no reading order, no worked project, and meets ownership
on a page that assumes the pages around it. *The Rust Programming Language* (https://doc.rust-lang.org/book) is the
model the owner named. What it does that the guide does not:

- one numbered order, read front to back, each chapter using only what earlier chapters taught;
- chapters cut into short sections (1.1, 1.2, …), one idea each;
- projects that put the ideas to work: a small one in chapter 2, larger ones in the middle and at the end;
- listings that are numbered, captioned, and shown with what they print or the error they give;
- a title page that says which compiler the text is for;
- appendices that turn the book into a reference: keywords, operators, tools.

An AI model is now as common a reader as a person, and it reads differently: it takes one section out of context,
searches by an error code, and copies a listing whole. The Rust Book was not written for that. This one is.

## 2. Decision

A second site, **The CeeSafe Programming Language** ("the book"), beside the documentation site and linked from it.

- **Source:** `book/` in this repository, Markdown, one file for each section.
- **Published:** `https://ceesafe.github.io/CeeSafe/book/`, by the workflow that publishes the site.
- **The guide stays.** The book does not replace a guide page; where both cover a topic the book teaches it and the
  guide page remains the quick answer. The specification stays the authority: where the book and `spec/` differ, the
  book is wrong.
- **Every listing is checked,** as the guide's are, and the check is a contract test.

## 3. The reader

Two readers, and one text for both.

**A person learning CeeSafe** who can program in some language. Not assumed: Rust, C, or systems programming.

**An AI model** asked to write or fix CeeSafe. It knows other languages well and CeeSafe little or wrongly, and it
will read a section, a listing or an appendix row on its own.

What that asks of the writing, for both:

1. **A section stands alone.** It names what it needs from earlier sections and links to them; it does not say
   "as we saw".
2. **A listing is a whole program** that compiles, with its exact output or its exact diagnostic beneath it. No
   fragment that only works inside text the reader cannot see.
3. **Rules are stated as rules,** in a marked box, in one or two sentences, with the specification section they come
   from. A model quotes a rule; a person rereads it.
4. **Mistakes are shown.** Each section that has a common mistake shows the wrong code, the diagnostic with its code
   (`N0241`), and the fix. The code is how a model, and a person with an error on the screen, finds the section.
5. **One term for one thing,** defined once in the glossary. "Borrow", not "reference" in one chapter and "pointer"
   in the next.
6. **Nothing borrowed from another language without saying so.** Where CeeSafe differs from what a Rust, C# or Go
   programmer expects, a "Coming from …" note says so beside the rule, since that is where a model goes wrong.

## 4. The book's parts

### 4.1 The reading order

Provisional: each chapter is written against what the compiler accepts, and a section whose feature is missing or
different is changed here, not worked around in the text. The guide page each draws on is in brackets.

| | Chapter | Sections |
|---|---|---|
| | **The CeeSafe Programming Language** | the title page: the version the text assumes, how to read it offline, how a model should use it |
| | **Introduction** | who it is for; how the book is laid out; how to read the listings |
| 1 | **Getting started** [installation, getting-started] | 1.1 Installation · 1.2 Hello, world · 1.3 Hello, package: `ceesafe.toml`, `check`, `run`, `build`, `fmt` |
| 2 | **Programming a guessing game** | one program built in steps: input, a random number, comparison, a loop, a bad guess handled |
| 3 | **Common programming concepts** [basics] | 3.1 Variables: `let`, `var`, `const` · 3.2 Data types · 3.3 Functions · 3.4 Comments · 3.5 Control flow |
| 4 | **Understanding ownership** [ownership] | 4.1 What ownership is: moves, copies, drops · 4.2 Borrowing: `borrow` and `borrow mut` · 4.3 Slices and views |
| 5 | **Structs** [structs-and-enums] | 5.1 Defining and making structs · 5.2 An example program · 5.3 Methods |
| 6 | **Enums and pattern matching** [structs-and-enums] | 6.1 Defining an enum · 6.2 `match` · 6.3 Optionals: `T?`, `??`, and the short forms |
| 7 | **Packages and files** [packages] | 7.1 A package and its manifest · 7.2 Files and `public` · 7.3 Imports · 7.4 Dependencies and the lock file |
| 8 | **Common collections** [collections] | 8.1 Vectors · 8.2 Strings and string views: UTF-8 · 8.3 Hash maps and ordered maps |
| 9 | **Error handling** [errors] | 9.1 Panics · 9.2 `Result`, `?` and `!` · 9.3 Allocation failure is a value · 9.4 To panic or not |
| 10 | **Generics, interfaces and regions** [generics, ownership] | 10.1 Generic types and functions · 10.2 Interfaces · 10.3 Regions: borrows inside structs |
| 11 | **Writing tests** [testing] | 11.1 How to write tests · 11.2 Running them · 11.3 Organising them; failure sweeps |
| 12 | **A project: a command-line program** | arguments, reading a file, refactoring, test-first, the environment, standard error |
| 13 | **Closures and iteration** [closures] | 13.1 Closures · 13.2 Iteration: `for` and `next` · 13.3 The project improved · 13.4 What they cost |
| 14 | **More about packages** [packages, ceesafec] | 14.1 Build settings · 14.2 Publishing · 14.3 Libraries and objects for other programs |
| 15 | **Memory** [memory] | 15.1 `Box` · 15.2 Destruction · 15.3 Allocators, arenas and scratch frames · 15.4 Cells: `Cell`, `RefCell`, `OnceCell` · 15.5 Shared ownership: `Arc` |
| 16 | **Threads** [threads] | 16.1 Starting threads · 16.2 Channels · 16.3 Shared state: `Mutex`, atomics · 16.4 `Send` and `Share` |
| 17 | **Async** [async] | 17.1 Tasks and `await` · 17.2 Scopes · 17.3 Timeouts, races and cancellation · 17.4 Async I/O · 17.5 Tasks and threads together |
| 18 | **Interfaces as values** [generics] | 18.1 Dynamic dispatch · 18.2 When to use it · 18.3 A design worked through |
| 19 | **Patterns** [structs-and-enums] | 19.1 Where patterns go · 19.2 Patterns that can fail · 19.3 The syntax, whole |
| 20 | **Advanced features** [unsafe-and-c, generics, basics] | 20.1 Unsafe code · 20.2 Calling C and being called by it · 20.3 More on interfaces and generics: const parameters, associated types · 20.4 Arithmetic: checked, wrapping, conversions · 20.5 Derives and macros |
| 21 | **A final project: a web server** | single-threaded; then tasks; shutdown and cleanup |
| 22 | **Without an operating system** [kernel] | 22.1 The freestanding profile · 22.2 Kernel code: contexts and effects · 22.3 CeeSafe inside a host program |

Chapters 1 to 12 are the course; a reader who stops there can write programs. Chapters 13 to 22 are read as needed.
Chapter 22 has no counterpart in the Rust Book: it is what CeeSafe is for.

### 4.2 The appendices: the reference

| | Appendix | What it is |
|---|---|---|
| A | Keywords | every keyword, what it means, the section that teaches it |
| B | Operators and symbols | each with its meaning and the types it takes |
| C | Built-in types and their methods | one table for each type, generated from `lib/` where it can be |
| D | Derives | what can be derived, and what each gives |
| E | The `ceesafec` command | each command and flag |
| F | Diagnostics | every code, one line, and the section that explains the rule |
| G | Coming from another language | one table each: Rust, C and C++, Go, C#/Java/TypeScript, Zig |
| H | The grammar | the grammar of `spec/grammar.md`, with links into the chapters |
| I | For AI models | how the book is laid out for a model, the machine-readable files (§5), and the mistakes models make |
| J | Versions | what each release added, by section |
| | Glossary | every term, one definition |

Appendix F is the index a model needs most and a person with an error needs next most.

### 4.3 A section's shape

Every section has the same parts, in this order, so that a reader of either kind knows where to look:

1. **What this section teaches,** one sentence, and what it assumes (links).
2. **The idea,** in prose, with the first listing early.
3. **Listings,** numbered by chapter ("Listing 4-3"), each with a caption, its file name, and its output or
   diagnostic.
4. **Rules** in boxes, each with its specification reference.
5. **Mistakes:** wrong code, the diagnostic, the fix.
6. **Coming from …** notes, where a habit from another language misleads.
7. **Summary:** the rules again, as a list, and what comes next.

A section is at most about 1,500 words. A longer one is two sections.

## 5. For a model: the machine-readable book

Built with the site, from the same files:

| File | What it holds |
|---|---|
| `/book/llms.txt` | the table of contents, each section with its one-sentence summary and its URL |
| `/book/llms-full.txt` | the whole book as one Markdown document |
| `/book/<section>.md` | each section as Markdown, at the page's URL with `.md` |
| `/book/index.json` | sections (id, title, summary, what it assumes, keywords, diagnostics, specification references, the version that introduced it) and listings (id, file, caption, output or diagnostic) |
| `/book/listings/…` | each listing as a file that compiles |

- **Stable identifiers.** A section's id (`ch04-02-borrowing`) and a rule's anchor do not change when a title does.
  A listing's number may change; its id does not.
- **A header in each source file** (front matter) holds what `index.json` holds, so the page and the index cannot
  disagree, and the check (§6) tests it: a diagnostic named there must be one a listing of the section produces.
- **Each page has a "Markdown" link** to its `.md`, beside the print link.
- **Later, in `ceesafec`:** the MCP server has `ceesafe_spec_search` for the specification. A `ceesafe_book_section`
  tool, answering from the book embedded in the compiler as the specification is, would let a model look a rule up
  where it learns the language. Not part of this decision: it needs its own ADR, and the book first.

## 6. How it is kept true

- **`tools/guide_check.py --guide book`** compiles and runs every listing, as it does the guide's, with the same
  comments (`fail`, `warns`, `panics`, `freestanding`, `check`). A listing is a whole file; where a chapter builds one
  program in steps, each step is a whole file under `book/listings/`.
- **The projects** (chapters 2, 12 and 21) are packages under `book/projects/`, built and tested by the check, and
  the text includes their files, not copies of them.
- **A new check, `tools/book_check.py`:** every section has its header and its parts in order; every link, listing
  number and section id resolves; every diagnostic code of Appendix F exists in the compiler and points to a section
  that shows it; the keyword and operator tables match the compiler's own lists; `llms.txt`, `index.json` and the
  pages agree.
- Both run in `tools/contract_tests.py`. A release whose compiler rejects a listing does not go out.
- **The title page's version** is the compiler's, written by the build, not by hand.

## 7. The site

Modelled on the Rust Book's, which is plain on purpose:

- **Left:** the numbered table of contents, chapters that fold, the current section marked. It folds away.
- **Top bar:** fold the contents, choose a theme (light, dark, and the reader's own setting by default), search,
  print the whole book, the Markdown of this page, the repository.
- **The page:** one column of text at a readable width. Listings have a header (the file's name and the listing's
  number), a copy button that copies the whole file, and their output in a block that looks different from code.
  Rule, mistake and "coming from" boxes each have one look.
- **Bottom and sides:** previous and next, and the left and right arrow keys.
- No landing page, no marketing, nothing that moves.

**Built with** one of two tools, the owner's choice (§8):

| | VitePress, as the site is | mdBook, as the Rust Book is |
|---|---|---|
| Toolchain | the one the site has (Node, `site/package.json`) | a second one (a Rust binary in the docs workflow) |
| CeeSafe syntax colouring | the site's grammar, as it is | a new one for mdBook's highlighter |
| The Rust Book's look | a theme to write: the numbered sidebar, the top bar, the arrows | as it comes |
| Print the whole book, search | search is there; a print page to write | both as they come |
| The machine-readable files of §5 | a build script, as `site/scripts/prepare.mjs` is | a preprocessor or a script beside it |

Recommended: **VitePress**, in a second configuration (`book/.vitepress/`), for one toolchain and the grammar that
exists. The look is a theme of a few hundred lines; the content does not depend on the choice, since both read
Markdown.

## 8. What the owner decides

1. **The tool:** VitePress (recommended) or mdBook.
2. **The title and the authors' line:** "The CeeSafe Programming Language", by whom.
3. **The licence of the text.** The compiler's licence covers code; a book usually states its own.
4. **The order of work.** Proposed: the skeleton, the site and both checks first, with chapter 1 and one section of
   chapter 4 as the pattern; then chapters 2 to 12 (the course); then 13 to 22; then the appendices, Appendix F and
   the machine-readable files as each chapter lands, not at the end.
5. **Whether a chapter may wait for the language.** Some sections will find a gap (a missing library function, a
   rough diagnostic). The rule of `AGENTS.md` applies: the gap is fixed in the compiler through an ADR, and the
   section waits; it is not written around.

## 9. What it costs, and what is not decided

- **Size.** The Rust Book is about 200,000 words. The guide's 33,000 are a start for perhaps a third of the
  chapters, and they are written as reference pages, so they are material, not text to move. This is the largest
  piece of writing the project has taken on.
- **Upkeep.** Every language change that touches a rule touches a section. The checks of §6 find a listing that no
  longer compiles; they do not find prose that has become untrue. Each ADR's checklist gains a line: the book's
  section.
- **Not decided here:** translations; a printed form; an interactive playground in the page; the MCP tool of §5.
