// The book's table of contents (Decision 0011 §4): the one list the sidebar, llms.txt and index.json are made from.
// A chapter or section with a `file` whose page exists (book/src/<file>.md) is written; any other is planned, and
// shown without a link. A section's `file` is its stable identifier: it does not change when the title does.
import { existsSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const source = join(dirname(fileURLToPath(import.meta.url)), "src");

/** Whether the page of this entry is written. */
export const written = (entry) => entry.file !== undefined && existsSync(join(source, `${entry.file}.md`));

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
  { title: "Programming a guessing game", file: "ch02-00-guessing-game", sections: [] },
  {
    title: "Common programming concepts",
    file: "ch03-00-common-concepts",
    sections: [
      { title: "Variables", file: "ch03-01-variables" },
      { title: "Data types", file: "ch03-02-data-types" },
      { title: "Functions", file: "ch03-03-functions" },
      { title: "Comments", file: "ch03-04-comments" },
      { title: "Control flow", file: "ch03-05-control-flow" },
    ],
  },
  {
    title: "Understanding ownership",
    file: "ch04-00-ownership",
    sections: [
      { title: "What ownership is", file: "ch04-01-what-ownership-is" },
      { title: "Borrowing", file: "ch04-02-borrowing" },
      { title: "Slices and views", file: "ch04-03-slices-and-views" },
    ],
  },
  {
    title: "Structs",
    file: "ch05-00-structs",
    sections: [
      { title: "Defining and making structs", file: "ch05-01-defining-structs" },
      { title: "An example program", file: "ch05-02-example-structs" },
      { title: "Methods", file: "ch05-03-methods" },
    ],
  },
  {
    title: "Enums and pattern matching",
    file: "ch06-00-enums",
    sections: [
      { title: "Defining an enum", file: "ch06-01-defining-an-enum" },
      { title: "match", file: "ch06-02-match" },
      { title: "Optionals", file: "ch06-03-optionals" },
    ],
  },
  {
    title: "Packages and files",
    file: "ch07-00-packages",
    sections: [
      { title: "A package and its manifest", file: "ch07-01-package-and-manifest" },
      { title: "Files and public", file: "ch07-02-files-and-public" },
      { title: "Imports", file: "ch07-03-imports" },
      { title: "Dependencies and the lock file", file: "ch07-04-dependencies" },
    ],
  },
  {
    title: "Common collections",
    file: "ch08-00-collections",
    sections: [
      { title: "Vectors", file: "ch08-01-vectors" },
      { title: "Strings and string views", file: "ch08-02-strings" },
      { title: "Hash maps and ordered maps", file: "ch08-03-maps" },
    ],
  },
  {
    title: "Error handling",
    file: "ch09-00-error-handling",
    sections: [
      { title: "Panics", file: "ch09-01-panics" },
      { title: "Result, ? and !", file: "ch09-02-result" },
      { title: "Allocation failure is a value", file: "ch09-03-allocation-failure" },
      { title: "To panic or not", file: "ch09-04-to-panic-or-not" },
    ],
  },
  {
    title: "Generics, interfaces and regions",
    file: "ch10-00-generics",
    sections: [
      { title: "Generic types and functions", file: "ch10-01-generic-types" },
      { title: "Interfaces", file: "ch10-02-interfaces" },
      { title: "Regions", file: "ch10-03-regions" },
    ],
  },
  {
    title: "Writing tests",
    file: "ch11-00-testing",
    sections: [
      { title: "How to write tests", file: "ch11-01-writing-tests" },
      { title: "Running them", file: "ch11-02-running-tests" },
      { title: "Organising them", file: "ch11-03-organising-tests" },
    ],
  },
  {
    title: "A project: a command-line program",
    file: "ch12-00-command-line-project",
    sections: [
      { title: "Accepting command-line arguments", file: "ch12-01-arguments" },
      { title: "Reading a file", file: "ch12-02-reading-a-file" },
      { title: "Refactoring for clarity and errors", file: "ch12-03-refactoring" },
      { title: "Adding the search, test first", file: "ch12-04-test-first" },
      { title: "Working with the environment", file: "ch12-05-environment" },
      { title: "Errors to standard error", file: "ch12-06-standard-error" },
    ],
  },
  {
    title: "Closures and iteration",
    file: "ch13-00-closures-and-iteration",
    sections: [
      { title: "Closures", file: "ch13-01-closures" },
      { title: "Iteration", file: "ch13-02-iteration" },
      { title: "The project improved", file: "ch13-03-project-improved" },
      { title: "What they cost", file: "ch13-04-what-they-cost" },
    ],
  },
  {
    title: "More about packages",
    file: "ch14-00-more-about-packages",
    sections: [
      { title: "Build settings", file: "ch14-01-build-settings" },
      { title: "Publishing", file: "ch14-02-publishing" },
      { title: "Libraries and objects for other programs", file: "ch14-03-libraries-for-other-programs" },
    ],
  },
  {
    title: "Memory",
    file: "ch15-00-memory",
    sections: [
      { title: "Box", file: "ch15-01-box" },
      { title: "Destruction", file: "ch15-02-destruction" },
      { title: "Allocators, arenas and scratch frames", file: "ch15-03-allocators" },
      { title: "Cells", file: "ch15-04-cells" },
      { title: "Shared ownership", file: "ch15-05-shared-ownership" },
    ],
  },
  {
    title: "Threads",
    file: "ch16-00-threads",
    sections: [
      { title: "Starting threads", file: "ch16-01-starting-threads" },
      { title: "Channels", file: "ch16-02-channels" },
      { title: "Shared state", file: "ch16-03-shared-state" },
      { title: "Send and Share", file: "ch16-04-send-and-share" },
    ],
  },
  {
    title: "Async",
    file: "ch17-00-async",
    sections: [
      { title: "Tasks and await", file: "ch17-01-tasks-and-await" },
      { title: "Scopes", file: "ch17-02-scopes" },
      { title: "Timeouts, races and cancellation", file: "ch17-03-timeouts-races-cancellation" },
      { title: "Async I/O", file: "ch17-04-async-io" },
      { title: "Tasks and threads together", file: "ch17-05-tasks-and-threads" },
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
    if (written(chapter)) {
      pages.push({ title: chapter.title, file: chapter.file, number: `${c + 1}` });
    }
    chapter.sections.forEach((section, s) => {
      if (written(section)) {
        pages.push({ title: section.title, file: section.file, number: `${c + 1}.${s + 1}` });
      }
    });
  });
  return pages;
}
