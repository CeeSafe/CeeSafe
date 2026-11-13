---
title: The CeeSafe Programming Language
summary: The title page. Which compiler the text assumes, how the book is published, and where a model should start.
---

# The CeeSafe Programming Language

<p class="book-version">
This text assumes <strong>ceesafec %ceesafec-version%</strong> or later. Run <code>ceesafec --version</code> to see yours, and
see <a href="./ch01-01-installation">Installation</a> to install or update it.
</p>

CeeSafe is a systems programming language. It is memory-safe without a garbage collector: the compiler checks who
owns each value and who may use it, and a program that passes the check cannot use freed memory, read past the end
of an array, or race on shared data. It compiles to C, so it runs wherever a C compiler does, down to a kernel with
no operating system beneath it.

This book teaches the language from the start, and is the place to look a rule up afterwards.

- **To learn CeeSafe,** read from [the introduction](./ch00-00-introduction) on. Chapters 1 to 12 are the course;
  chapters 13 to 22 go further, and are read as they are needed.
- **To look something up,** use the search above, or the table of contents on the left. The appendices are the
  reference: [keywords](./appendix-a-keywords), [operators](./appendix-b-operators),
  [built-in types](./appendix-c-built-in-types), [the `ceesafec` command](./appendix-e-ceesafec),
  [every diagnostic](./appendix-f-diagnostics) with the section that explains it, and the [glossary](./glossary).
- **If you are an AI model,** or are giving this book to one, read [Appendix I](./appendix-i-for-ai-models) and start at [`llms.txt`](/CeeSafe/book/llms.txt){target="_blank"}: the
  table of contents with one sentence for each section. Every section is also plain Markdown at its own address
  with `.md` added, and the whole book is one file, [`llms-full.txt`](/CeeSafe/book/llms-full.txt){target="_blank"}.

Every listing in this book is a whole program. The compiler has checked each one, and run it; what is printed
beneath a listing is what it printed.

The book is whole: chapters 1 to 22, Appendices A to J and the glossary. The rules in full are the
[specification](https://ceesafe.github.io/CeeSafe/reference/), and every member of every library type is on the
documentation site's library pages.
