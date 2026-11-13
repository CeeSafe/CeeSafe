// Writes the machine-readable book (Decision 0011 §5) into src/public/, from the pages and the table of contents:
// - llms.txt: the table of contents, each written section with its one-sentence summary and its URL;
// - llms-full.txt: every written page as one Markdown document, in reading order;
// - <file>.md: each page's Markdown at the page's URL with ".md";
// - index.json: the sections (their headers) and the listings (number, caption, file, output or diagnostic);
// - listings/<file>-<n>.cee: each listing as a file.
// It fails when a written page has no header, or its header lacks a title or a summary.
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { appendices, chapters, written, writtenPages } from "../toc.mjs";
import { withVersion } from "../version.mjs";

const book = join(dirname(fileURLToPath(import.meta.url)), "..");
const source = join(book, "src");
const output = join(source, "public");
const base = "https://ceesafe.github.io/CeeSafe/book/";

/** A page's header (the `key: value` lines and `key: [a, b]` lists between the first two `---`) and its body. */
function parse(file) {
  const text = withVersion(readFileSync(join(source, `${file}.md`), "utf8"));
  const match = text.match(/^---\n([\s\S]*?)\n---\n/);
  if (!match) {
    throw new Error(`book/src/${file}.md has no header`);
  }
  const header = {};
  for (const line of match[1].split("\n")) {
    const entry = line.match(/^([a-z]+):\s*(.*)$/);
    if (!entry) {
      continue;
    }
    const value = entry[2].trim();
    header[entry[1]] = value.startsWith("[")
      ? value.slice(1, -1).split(",").map((item) => item.trim().replace(/^"|"$/g, "")).filter((item) => item !== "")
      : value.replace(/^"|"$/g, "");
  }
  for (const key of ["title", "summary"]) {
    if (!header[key]) {
      throw new Error(`book/src/${file}.md: its header has no '${key}'`);
    }
  }
  return { header, body: text.slice(match[0].length) };
}

/** The listings of a page: a caption line `<span class="listing-caption">…Listing N-M…</span>`, then a code block,
 *  then perhaps a `text` block with what it prints or the compiler says. */
function listingsOf(file, body) {
  const listings = [];
  const pattern = /<span class="listing-caption"><strong>Listing ([0-9]+-[0-9]+)<\/strong>(?:, <code>([^<]+)<\/code>)?: (.*?)<\/span>\s*\n+(?:<!-- ceesafe: ([^>]*?) -->\n)?```([a-z]*)\n([\s\S]*?)\n```(?:\s*\n+```text\n([\s\S]*?)\n```)?/g;
  for (const match of body.matchAll(pattern)) {
    const [, number, name, caption, directive, language, code, result] = match;
    const extension = language === "ceesafe" ? "cee" : language === "" || language === "text" ? "txt" : language;
    const path = `listings/${file}-${number}.${extension}`;
    listings.push({
      number,
      section: file,
      file: name ?? "main.cee",
      language: language || "text",
      caption: caption.replace(/<[^>]+>/g, ""),
      check: directive?.trim() ?? "runs",
      output: result ?? null,
      url: base + path,
    });
    writeFileSync(join(output, path), code + "\n");
  }
  return listings;
}

rmSync(output, { recursive: true, force: true });
mkdirSync(join(output, "listings"), { recursive: true });

const pages = writtenPages().map((page) => ({ ...page, ...parse(page.file) }));
const urlOf = (file) => base + (file === "index" ? "" : file);

const sections = [];
const listings = [];
let full = "";
for (const page of pages) {
  const markdown = `# ${page.number ? page.number + ". " : ""}${page.header.title}\n\n${page.body.replace(/^# .*\n/, "").trim()}\n`;
  writeFileSync(join(output, `${page.file}.md`), markdown);
  full += `${markdown}\n---\n\n`;
  const found = listingsOf(page.file, page.body);
  listings.push(...found);
  sections.push({
    id: page.file,
    number: page.number,
    title: page.header.title,
    summary: page.header.summary,
    assumes: page.header.assumes ?? [],
    keywords: page.header.keywords ?? [],
    diagnostics: page.header.diagnostics ?? [],
    specification: page.header.specification ?? [],
    listings: found.map((listing) => listing.number),
    url: urlOf(page.file),
    markdown: `${base}${page.file}.md`,
  });
}

// What the pages must satisfy (headers, numbering, diagnostics, links) is checked by tools/book_check.py.

const planned = [];
chapters.forEach((chapter, c) => {
  if (!written(chapter)) {
    planned.push(`${c + 1}. ${chapter.title}`);
  }
  chapter.sections.forEach((section, s) => {
    if (!written(section)) {
      planned.push(`${c + 1}.${s + 1}. ${section.title}`);
    }
  });
});

const lines = [
  "# The CeeSafe Programming Language",
  "",
  "> The book: CeeSafe taught from the start, each section standing alone, every listing a whole program that the",
  "> compiler has checked, shown with what it prints or the diagnostic it gets. CeeSafe is a memory-safe systems",
  "> language compiled to C; code that looks right from Rust, C# or TypeScript is often not CeeSafe.",
  "",
  `- [The whole book as one document](${base}llms-full.txt)`,
  `- [Sections and listings as JSON](${base}index.json)`,
  "",
  "## Sections",
  "",
  ...sections.map((s) => `- [${s.number ? s.number + ". " : ""}${s.title}](${s.markdown}): ${s.summary}`),
  "",
  "## Planned, not written yet",
  "",
  ...planned.map((title) => `- ${title}`),
  ...appendices.map((appendix) => `- Appendix ${appendix.title}`),
  "",
];
writeFileSync(join(output, "llms.txt"), lines.join("\n"));
writeFileSync(join(output, "llms-full.txt"), full);
writeFileSync(join(output, "index.json"), JSON.stringify({ book: "The CeeSafe Programming Language", base, sections, listings }, null, 2) + "\n");

if (!existsSync(join(output, "llms.txt"))) {
  throw new Error("nothing was written");
}
console.log(`prepare: ${sections.length} pages, ${listings.length} listings, ${planned.length} sections planned`);
