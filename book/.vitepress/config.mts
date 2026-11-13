import { readFileSync } from "node:fs";
import { defineConfig } from "vitepress";
import { appendices, chapters, front, written } from "../toc.mjs";
import { withVersion } from "../version.mjs";

// The book (Decision 0011): a second site beside the documentation site, in the Rust Book's shape. CeeSafe code is
// highlighted by the VS Code extension's grammar, as on the documentation site.
const ceesafe = JSON.parse(readFileSync(new URL("../../editors/vscode/syntaxes/ceesafe.tmLanguage.json", import.meta.url), "utf8"));

const link = (file: string) => (file === "index" ? "/" : `/${file}`);

// A numbered table of contents. A page that is not written yet has no link, so it shows as plain text.
const sidebar = [
  { items: front.map((page) => ({ text: page.title, link: link(page.file) })) },
  ...chapters.map((chapter, c) => ({
    text: `${c + 1}. ${chapter.title}`,
    link: written(chapter) ? link(chapter.file) : undefined,
    collapsed: !written(chapter) && !chapter.sections.some((s) => written(s)),
    items: chapter.sections.map((section, s) => ({
      text: `${c + 1}.${s + 1}. ${section.title}`,
      link: written(section) ? link(section.file) : undefined,
    })),
  })),
  { text: "Appendices", collapsed: true, items: appendices.map((appendix) => ({ text: appendix.title })) },
];

export default defineConfig({
  title: "The CeeSafe Programming Language",
  description: "The book: learn CeeSafe from the start, and look its rules up.",
  base: "/CeeSafe/book/",
  srcDir: "src",
  cleanUrls: true,
  head: [["link", { rel: "icon", type: "image/svg+xml", href: "/CeeSafe/logo.svg" }]],
  markdown: {
    languages: [{ ...ceesafe, name: "ceesafe", aliases: ["CeeSafe", "cee"] }],
    // The compiler's version where a page writes %ceesafec-version% (version.mjs).
    config: (md) => {
      md.core.ruler.before("normalize", "ceesafec-version", (state) => {
        state.src = withVersion(state.src);
      });
    },
  },
  themeConfig: {
    siteTitle: "The CeeSafe Book",
    nav: [
      { text: "Documentation", link: "https://ceesafe.github.io/CeeSafe/" },
      { text: "For AI", link: "/llms.txt", target: "_blank" },
    ],
    sidebar,
    socialLinks: [{ icon: "github", link: "https://github.com/CeeSafe/CeeSafe" }],
    search: { provider: "local" },
    outline: { level: [2, 3], label: "In this section" },
    docFooter: { prev: "Previous", next: "Next" },
    footer: {
      message: "Chapters 1 to 22 are written; the appendices are to come.",
      copyright: "Copyright © 2026 Veeran Puthumkara.",
    },
  },
});
