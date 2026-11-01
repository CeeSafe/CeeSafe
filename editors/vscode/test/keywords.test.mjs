// The grammar names exactly the compiler's keywords (spec/tooling.md §2.4): the words of
// src/syntax/token.cee's keywordAt, spelled by its keywordText.
import { readFileSync } from "node:fs";
import { expect, test } from "vitest";

const token = readFileSync(new URL("../../../src/syntax/token.cee", import.meta.url), "utf8");
const grammar = JSON.parse(
  readFileSync(new URL("../syntaxes/ceesafe.tmLanguage.json", import.meta.url), "utf8"),
);

/** The arms of the `match` in function `name`, as [left, right] pairs of source text. */
function arms(name) {
  const start = token.indexOf(`fn ${name}(`);
  const body = token.slice(start, token.indexOf("\n};", start));
  return [...body.matchAll(/^\s+(\S+) => (.+),$/gm)].map((m) => [m[1], m[2]]);
}

test("the grammar's keywords are the compiler's", () => {
  const text = new Map(arms("keywordText").map(([kind, word]) => [kind, JSON.parse(word)]));
  const compiler = arms("keywordAt").map(([, kind]) => text.get(kind));
  expect(compiler).toHaveLength(46);
  const named = grammar.repository.keywords.patterns.flatMap((p) =>
    /^\\b\(([A-Za-z|]+)\)\\b$/.exec(p.match)[1].split("|"),
  );
  expect(named.sort()).toEqual(compiler.sort());
});
