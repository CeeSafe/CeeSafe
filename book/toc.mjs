// The book's table of contents (Decision 0011 §4): the one list the sidebar, llms.txt and index.json are made from.
// A chapter or section with a `file` is written (book/src/<file>.md); one without is planned, and shown without a
// link. A section's `file` is its stable identifier: it does not change when the title does.

export const front = [
  { title: "The CeeSafe Programming Language", file: "index" },
  { title: "Introduction", file: "ch00-00-introduction" },
];

export const chapters = [
  {
    title: "Getting started",
    file: "ch01-00-getting-started",
    sections: [
      { title: "Installation", file: "ch01-01-installation" },
      { title: "Hello, world", file: "ch01-02-hello-world" },
      { title: "Hello, package", file: "ch01-03-hello-package" },
    ],
  },
  { title: "Programming a guessing game", sections: [] },
  {
    title: "Common programming concepts",
    sections: [
      { title: "Variables" },
      { title: "Data types" },
      { title: "Functions" },
      { title: "Comments" },
      { title: "Control flow" },
    ],
  },
  {
    title: "Understanding ownership",
    sections: [
      { title: "What ownership is" },
      { title: "Borrowing", file: "ch04-02-borrowing" },
      { title: "Slices and views" },
    ],
  },
  { title: "Structs", sections: [{ title: "Defining and making structs" }, { title: "An example program" }, { title: "Methods" }] },
  { title: "Enums and pattern matching", sections: [{ title: "Defining an enum" }, { title: "match" }, { title: "Optionals" }] },
  {
    title: "Packages and files",
    sections: [
      { title: "A package and its manifest" },
      { title: "Files and public" },
      { title: "Imports" },
      { title: "Dependencies and the lock file" },
    ],
  },
  { title: "Common collections", sections: [{ title: "Vectors" }, { title: "Strings and string views" }, { title: "Hash maps and ordered maps" }] },
  {
    title: "Error handling",
    sections: [{ title: "Panics" }, { title: "Result, ? and !" }, { title: "Allocation failure is a value" }, { title: "To panic or not" }],
  },
  { title: "Generics, interfaces and regions", sections: [{ title: "Generic types and functions" }, { title: "Interfaces" }, { title: "Regions" }] },
  { title: "Writing tests", sections: [{ title: "How to write tests" }, { title: "Running them" }, { title: "Organising them" }] },
  { title: "A project: a command-line program", sections: [] },
  { title: "Closures and iteration", sections: [{ title: "Closures" }, { title: "Iteration" }, { title: "The project improved" }, { title: "What they cost" }] },
  { title: "More about packages", sections: [{ title: "Build settings" }, { title: "Publishing" }, { title: "Libraries and objects for other programs" }] },
  {
    title: "Memory",
    sections: [{ title: "Box" }, { title: "Destruction" }, { title: "Allocators, arenas and scratch frames" }, { title: "Cells" }, { title: "Shared ownership" }],
  },
  { title: "Threads", sections: [{ title: "Starting threads" }, { title: "Channels" }, { title: "Shared state" }, { title: "Send and Share" }] },
  {
    title: "Async",
    sections: [
      { title: "Tasks and await" },
      { title: "Scopes" },
      { title: "Timeouts, races and cancellation" },
      { title: "Async I/O" },
      { title: "Tasks and threads together" },
    ],
  },
  { title: "Interfaces as values", sections: [{ title: "Dynamic dispatch" }, { title: "When to use it" }, { title: "A design worked through" }] },
  { title: "Patterns", sections: [{ title: "Where patterns go" }, { title: "Patterns that can fail" }, { title: "The syntax, whole" }] },
  {
    title: "Advanced features",
    sections: [
      { title: "Unsafe code" },
      { title: "Calling C and being called by it" },
      { title: "More on interfaces and generics" },
      { title: "Arithmetic: checked, wrapping, conversions" },
      { title: "Derives and macros" },
    ],
  },
  { title: "A final project: a web server", sections: [] },
  {
    title: "Without an operating system",
    sections: [{ title: "The freestanding profile" }, { title: "Kernel code" }, { title: "CeeSafe inside a host program" }],
  },
];

export const appendices = [
  { title: "A: Keywords" },
  { title: "B: Operators and symbols" },
  { title: "C: Built-in types and their methods" },
  { title: "D: Derives" },
  { title: "E: The ceesafec command" },
  { title: "F: Diagnostics" },
  { title: "G: Coming from another language" },
  { title: "H: The grammar" },
  { title: "I: For AI models" },
  { title: "J: Versions" },
  { title: "Glossary" },
];

/** Every written page in reading order, with its number ("1.2") where it has one. */
export function writtenPages() {
  const pages = front.map((p) => ({ ...p, number: "" }));
  chapters.forEach((chapter, c) => {
    if (chapter.file) {
      pages.push({ title: chapter.title, file: chapter.file, number: `${c + 1}` });
    }
    chapter.sections.forEach((section, s) => {
      if (section.file) {
        pages.push({ title: section.title, file: section.file, number: `${c + 1}.${s + 1}` });
      }
    });
  });
  return pages;
}
