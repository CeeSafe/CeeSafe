import { readdirSync, readFileSync } from "node:fs";
import { defineConfig } from "vitepress";

// CeeSafe code is highlighted by the VS Code extension's TextMate grammar (editors/vscode), so the site and the editor
// agree.
const ceesafe = JSON.parse(readFileSync(new URL("../../editors/vscode/syntaxes/ceesafe.tmLanguage.json", import.meta.url), "utf8"));

const guide = [
  {
    text: "Introduction",
    items: [
      { text: "What CeeSafe is", link: "/guide/introduction" },
      { text: "Installation", link: "/guide/installation" },
      { text: "Getting started", link: "/guide/getting-started" },
      { text: "For AI agents", link: "/guide/for-agents" },
      { text: "Built-in types and functions", link: "/guide/builtins" },
    ],
  },
  {
    text: "Coming from another language",
    collapsed: false,
    items: [
      { text: "Overview", link: "/guide/coming-from/" },
      { text: "Rust", link: "/guide/coming-from/rust" },
      { text: "Zig", link: "/guide/coming-from/zig" },
      { text: "C and C++", link: "/guide/coming-from/c-and-cpp" },
      { text: "C#, Java and TypeScript", link: "/guide/coming-from/csharp-java-typescript" },
      { text: "Go", link: "/guide/coming-from/go" },
    ],
  },
  {
    text: "The language",
    items: [
      { text: "Basics", link: "/guide/basics" },
      { text: "Structs, enums and patterns", link: "/guide/structs-and-enums" },
      { text: "Ownership and borrowing", link: "/guide/ownership" },
      { text: "Errors", link: "/guide/errors" },
      { text: "Memory and allocation", link: "/guide/memory" },
      { text: "Arrays, slices and collections", link: "/guide/collections" },
      { text: "Generics and interfaces", link: "/guide/generics" },
      { text: "Functions and closures", link: "/guide/closures" },
      { text: "Threads", link: "/guide/threads" },
      { text: "Async tasks", link: "/guide/async" },
      { text: "Unsafe code and C", link: "/guide/unsafe-and-c" },
      { text: "Kernel code", link: "/guide/kernel" },
    ],
  },
  {
    text: "Tools",
    items: [
      { text: "The ceesafec command", link: "/guide/ceesafec" },
      { text: "Packages", link: "/guide/packages" },
      { text: "Testing", link: "/guide/testing" },
      { text: "Formatting and documentation", link: "/guide/formatting-and-docs" },
      { text: "Editors", link: "/guide/editors" },
      { text: "Debugging", link: "/guide/debugging" },
      { text: "Performance", link: "/guide/performance" },
    ],
  },
  {
    text: "Built with CeeSafe",
    items: [{ text: "Clay: a UI toolkit", link: "/guide/clay" }],
  },
];

// The ADRs, by number and first heading, from spec/decisions/.
const decisionsDirectory = new URL("../../spec/decisions/", import.meta.url);
const decisions = readdirSync(decisionsDirectory)
  .filter((name) => /^\d{4}-.*\.md$/.test(name))
  .sort()
  .map((name) => {
    const heading = readFileSync(new URL(name, decisionsDirectory), "utf8").match(/^# (.+)$/m)?.[1] ?? name;
    return { text: heading.replace(/^ADR\s+/, ""), link: `/reference/decisions/${name.replace(/\.md$/, "")}` };
  });

const reference = [
  {
    text: "Specification",
    items: [
      { text: "Overview", link: "/reference/" },
      { text: "Language specification", link: "/reference/language-spec" },
      { text: "Grammar", link: "/reference/grammar" },
      { text: "Type system and semantics", link: "/reference/type-system" },
      { text: "Ownership: formal design", link: "/reference/ownership" },
      { text: "References and the borrow checker", link: "/reference/lifetimes" },
      { text: "Memory model", link: "/reference/memory-model" },
      { text: "Unsafe code", link: "/reference/unsafe" },
      { text: "C backend", link: "/reference/c-backend" },
      { text: "Optimisations", link: "/reference/optimisations" },
      { text: "Profiles", link: "/reference/profiles" },
      { text: "Diagnostics", link: "/reference/diagnostics" },
      { text: "Tooling", link: "/reference/tooling" },
      { text: "Formatting", link: "/reference/formatting" },
    ],
  },
  { text: "Library", items: [{ text: "lib/core", link: "/reference/api/core" }, { text: "lib/std", link: "/reference/api/std" }] },
  {
    text: "The compiler",
    collapsed: true,
    items: [
      { text: "Architecture", link: "/reference/architecture" },
      { text: "Intermediate representations", link: "/reference/intermediate-representation" },
      { text: "Testing", link: "/reference/testing" },
    ],
  },
  { text: "Decisions (ADRs)", collapsed: true, items: [{ text: "Index", link: "/reference/decisions/" }, ...decisions] },
];

export default defineConfig({
  title: "CeeSafe",
  description: "A memory-safe systems language with C#-style syntax, compiled to portable C.",
  base: "/CeeSafe/",
  srcExclude: ["README.md", "public/**"],
  cleanUrls: true,
  lastUpdated: true,
  head: [["link", { rel: "icon", type: "image/svg+xml", href: "/CeeSafe/logo.svg" }]],
  markdown: {
    languages: [{ ...ceesafe, name: "ceesafe", aliases: ["CeeSafe", "cee"] }],
  },
  themeConfig: {
    logo: "/logo.svg",
    nav: [
      { text: "Guide", link: "/guide/introduction", activeMatch: "/guide/" },
      { text: "Specification", link: "/reference/", activeMatch: "/reference/(?!api/|decisions/)" },
      { text: "Library", link: "/reference/api/core", activeMatch: "/reference/api/" },
      { text: "Decisions", link: "/reference/decisions/", activeMatch: "/reference/decisions/" },
    ],
    sidebar: { "/guide/": guide, "/reference/": reference },
    socialLinks: [{ icon: "github", link: "https://github.com/CeeSafe/CeeSafe" }],
    search: { provider: "local" },
    outline: { level: [2, 3] },
    editLink: {
      // Pages are copies of spec/ and guide/ (site/scripts/prepare.mjs): edit the originals.
      pattern: ({ filePath }) =>
        filePath.startsWith("reference/")
          ? `https://github.com/CeeSafe/CeeSafe/edit/main/spec/${filePath.slice("reference/".length).replace(/(^|\/)index\.md$/, "$1README.md")}`
          : `https://github.com/CeeSafe/CeeSafe/edit/main/${filePath}`,
      text: "Edit this page on GitHub",
    },
    footer: {
      message: "Pre-1.0: the language and its tools are still changing.",
      copyright: "GPL-2.0-only, with a runtime exception for the code CeeSafe puts into the programs it compiles. Copyright © 2026 Veeran Puthumkara.",
    },
  },
});
