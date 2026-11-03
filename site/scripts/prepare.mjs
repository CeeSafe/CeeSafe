// Copies the repository's spec/ and guide/ into the documentation site (site/README.md):
// - spec/ becomes reference/ (spec/README.md is reference/index.md), guide/ becomes guide/;
// - links between the two stay relative (guide/ → ../spec/x.md becomes ../reference/x.md); links to other files of
//   the repository become GitHub URLs;
// - outside code, `<` that does not start an HTML tag and `{{` are escaped, since VitePress compiles pages as Vue.
// It also writes llms.txt, llms-full.txt and each page's Markdown at its URL with ".md", for AI agents.
import { existsSync, mkdirSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { dirname, join, posix, relative, sep } from "node:path";
import { fileURLToPath } from "node:url";

const site = join(dirname(fileURLToPath(import.meta.url)), "..");
const repository = join(site, "..");
const github = "https://github.com/CeeSafe/CeeSafe";
const base = "https://ceesafe.github.io/CeeSafe/";

// Repository directory → site directory.
const trees = [
  ["spec", "reference"],
  ["guide", "guide"],
];

const htmlTags = new Set(["a", "br", "sub", "sup", "details", "summary", "img", "code", "kbd", "b", "i", "em", "strong", "div", "span", "p", "table", "tr", "td", "th"]);

function* markdownFiles(directory) {
  for (const name of readdirSync(directory).sort()) {
    const path = join(directory, name);
    if (statSync(path).isDirectory()) {
      yield* markdownFiles(path);
    } else if (name.endsWith(".md")) {
      yield path;
    }
  }
}

/** The site path of a repository Markdown file (POSIX), or null when it is not published. */
function published(repositoryPath) {
  for (const [from, to] of trees) {
    if (repositoryPath.startsWith(from + "/")) {
      const rest = repositoryPath.slice(from.length + 1).replace(/(^|\/)README\.md$/, "$1index.md");
      return `${to}/${rest}`;
    }
  }
  return null;
}

/** A link target rewritten for the site, from the repository file `from`. */
function rewriteLink(from, target) {
  if (/^[a-z]+:/i.test(target) || target.startsWith("#")) {
    return target;
  }
  const [path, anchor = ""] = target.split(/(?=#)/);
  let resolved = posix.normalize(posix.join(posix.dirname(from), decodeURI(path)));
  const directory = join(repository, ...resolved.split("/"));
  if (existsSync(directory) && statSync(directory).isDirectory() && published(resolved + "/README.md") !== null) {
    resolved = posix.join(resolved, "README.md"); // the directory's index page (written below when it has no README)
  }
  const page = resolved.endsWith(".md") ? published(resolved) : null;
  if (page !== null) {
    return posix.relative(posix.dirname(published(from)), page) + anchor;
  }
  const absolute = join(repository, ...resolved.split("/"));
  const kind = existsSync(absolute) && statSync(absolute).isDirectory() ? "tree" : "blob";
  return `${github}/${kind}/main/${resolved}${anchor}`;
}

/** Escapes text outside code for Vue, and rewrites links. */
function transform(from, text) {
  let fence = null;
  return text
    .split("\n")
    .map((line) => {
      const opening = line.match(/^\s*(```+|~~~+)/);
      if (opening) {
        if (fence === null) {
          fence = opening[1];
        } else if (line.trim().startsWith(fence)) {
          fence = null;
        }
        return line;
      }
      if (fence !== null) {
        return line;
      }
      return line
        .split(/(`+[^`]*`+)/)
        .map((part, index) =>
          index % 2 === 1
            ? part
            : part
                .replace(/\]\(([^)\s]+)\)/g, (_, target) => `](${rewriteLink(from, target)})`)
                .replace(/<(?!!)(\/?)([A-Za-z][\w-]*)?/g, (match, slash, tag) =>
                  tag && htmlTags.has(tag.toLowerCase()) ? match : "&lt;" + slash + (tag ?? ""),
                )
                .replace(/\{\{/g, "&#123;&#123;"),
        )
        .join("");
    })
    .join("\n");
}

const publicDirectory = join(site, "public");
const titleOf = (text, fallback) => text.match(/^# (.+)$/m)?.[1].trim() ?? fallback;
const textOf = (page) => readFileSync(join(publicDirectory, ...page.split("/")), "utf8");
for (const generated of ["guide", "reference", "llms.txt", "llms-full.txt"]) {
  rmSync(join(publicDirectory, generated), { recursive: true, force: true });
}
const pages = [];
for (const [from, to] of trees) {
  rmSync(join(site, to), { recursive: true, force: true });
  for (const file of markdownFiles(join(repository, from))) {
    const source = relative(repository, file).split(sep).join("/");
    const page = published(source);
    const text = readFileSync(file, "utf8");
    for (const [directory, content] of [[site, transform(source, text)], [publicDirectory, text]]) {
      const target = join(directory, ...page.split("/"));
      mkdirSync(dirname(target), { recursive: true });
      writeFileSync(target, content);
    }
    pages.push(page);
  }
}
// spec/decisions/ has no README: its index page lists the ADRs by their titles.
const decisions = pages.filter((page) => /^reference\/decisions\/\d{4}-/.test(page)).sort();
writeFileSync(
  join(site, "reference", "decisions", "index.md"),
  [
    "# Decisions",
    "",
    "ADRs 0001–0030 come from the earlier compiler CeeSafe began as a port of, with their text unchanged; CeeSafe's own language",
    "decisions continue the series from ADR 0031 ([the specification](../index.md) §2).",
    "",
    ...decisions.map((page) => `- [${titleOf(textOf(page), page)}](${posix.basename(page)})`),
    "",
  ].join("\n"),
);
console.log(`prepare: ${pages.length} pages from spec/ and guide/, and the decisions' index`);

// ---- For AI agents: llms.txt (an index) and llms-full.txt (the guide and the specification in one file). ----

const pageUrl = (page) => base + page;

/** The guide in reading order: the sidebar's (config.mts), then any page it does not list. */
const guideOrder = [
  "introduction", "installation", "getting-started", "for-agents",
  "basics", "structs-and-enums", "ownership", "errors", "memory", "collections", "generics", "closures", "threads",
  "unsafe-and-c", "kernel", "ceesafec", "packages", "formatting-and-docs", "editors", "debugging",
  "coming-from/index", "coming-from/rust", "coming-from/zig", "coming-from/c-and-cpp", "coming-from/csharp-java-typescript", "coming-from/go",
].map((name) => `guide/${name}.md`);
const guidePages = pages
  .filter((page) => page.startsWith("guide/"))
  .sort((a, b) => (guideOrder.indexOf(a) + 1 || 999) - (guideOrder.indexOf(b) + 1 || 999) || a.localeCompare(b));

/** The normative specification, in the order an agent needs it. */
const specification = ["grammar", "type-system", "diagnostics", "unsafe", "ownership", "lifetimes", "memory-model", "formatting", "tooling", "c-backend"]
  .map((name) => `reference/${name}.md`);

const summary = [
  "# CeeSafe",
  "",
  "> CeeSafe is a memory-safe systems programming language with C#- and TypeScript-style syntax, compiled ahead of",
  "> time to portable C11. Ownership and borrowing are checked at compile time; there is no garbage collector. Its",
  "> compiler, ceesafec, is written in CeeSafe.",
  "",
  "Agents writing CeeSafe: read the agent guide first, and use `ceesafec mcp` (an MCP server with a checker, fixes, a",
  "sandboxed interpreter and this documentation) or `ceesafec check --diagnostics-format=json` and `ceesafec fix`.",
  "",
];
writeFileSync(
  join(publicDirectory, "llms.txt"),
  [
    ...summary,
    "## Guide",
    "",
    ...guidePages.map((page) => `- [${titleOf(textOf(page), page)}](${pageUrl(page)})`),
    "",
    "## Specification",
    "",
    ...specification.map((page) => `- [${titleOf(textOf(page), page)}](${pageUrl(page)})`),
    "",
    "## Optional",
    "",
    `- [Everything above in one file](${base}llms-full.txt)`,
    `- [Library: lib/core](${pageUrl("reference/api/core.md")})`,
    `- [The specification's index](${pageUrl("reference/index.md")})`,
    "",
  ].join("\n"),
);
writeFileSync(
  join(publicDirectory, "llms-full.txt"),
  [...summary, ...[...guidePages, ...specification].map((page) => `\n<!-- ${pageUrl(page)} -->\n\n${textOf(page).trim()}\n`)].join("\n"),
);
console.log(`prepare: llms.txt (${guidePages.length} guide pages, ${specification.length} specifications) and llms-full.txt`);
