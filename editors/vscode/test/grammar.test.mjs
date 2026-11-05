// The TextMate grammar (spec/tooling.md §2.4), run by vscode-textmate as VS Code runs it: each sample token must get
// the scope a theme colours it by.
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
import { describe, expect, test } from "vitest";
import oniguruma from "vscode-oniguruma";
import textmate from "vscode-textmate";

const require = createRequire(import.meta.url);
const wasm = readFileSync(require.resolve("vscode-oniguruma/release/onig.wasm"));
await oniguruma.loadWASM(wasm.buffer);

const registry = new textmate.Registry({
  onigLib: Promise.resolve({
    createOnigScanner: (patterns) => new oniguruma.OnigScanner(patterns),
    createOnigString: (text) => new oniguruma.OnigString(text),
  }),
  loadGrammar: async () =>
    textmate.parseRawGrammar(
      readFileSync(new URL("../syntaxes/ceesafe.tmLanguage.json", import.meta.url), "utf8"),
      "ceesafe.tmLanguage.json",
    ),
});
const grammar = await registry.loadGrammar("source.ceesafe");

/** The innermost scope of each token of one line, as [text, scope] pairs (whitespace left out). */
function scopes(line) {
  const { tokens } = grammar.tokenizeLine(line, textmate.INITIAL);
  return tokens
    .map((token) => [line.slice(token.startIndex, token.endIndex), token.scopes.at(-1)])
    .filter(([text]) => text.trim().length > 0);
}

/** The scope of the first token whose text is `text`. */
function scopeOf(line, text) {
  return scopes(line).find(([t]) => t.trim() === text)?.[1];
}

describe("ceesafe.tmLanguage.json", () => {
  test.each([
    ["public fn length(p: Point) -> i32 => p.x;", "fn", "keyword.declaration.ceesafe"],
    ["public fn length(p: Point) -> i32 => p.x;", "public", "storage.modifier.ceesafe"],
    ["public fn length(p: Point) -> i32 => p.x;", "length", "entity.name.function.ceesafe"],
    ["public fn length(p: Point) -> i32 => p.x;", "Point", "entity.name.type.ceesafe"],
    ["public fn length(p: Point) -> i32 => p.x;", "i32", "support.type.primitive.ceesafe"],
    ["    if (n == 0) { return null; }", "if", "keyword.control.ceesafe"],
    ["    if (n == 0) { return null; }", "null", "constant.language.ceesafe"],
    ["    let v = borrow mut this.items;", "borrow", "storage.modifier.ceesafe"],
    ["    let v = borrow mut this.items;", "this", "variable.language.ceesafe"],
    ["@effects(sleep) fn wait();", "@effects", "entity.name.function.decorator.ceesafe"],
    ["let x = 0xFF_FFu32 + 1_000;", "0xFF_FFu32", "constant.numeric.integer.ceesafe"],
    ["let x = 6.02e23f64;", "6.02e23f64", "constant.numeric.float.ceesafe"],
    ["let k = 'k' as u8;", "'k'", "string.quoted.single.ceesafe"],
    ["let k = 'k' as u8;", "as", "keyword.operator.cast.ceesafe"],
  ])("%s: %s is %s", (line, text, scope) => {
    expect(scopeOf(line, text)).toBe(scope);
  });

  test("a string keeps its escapes and is not split by a comment marker", () => {
    expect(scopes('let s = "a\\n // b";')).toContainEqual([
      "\\n",
      "constant.character.escape.ceesafe",
    ]);
    expect(scopes('let s = "a\\n // b";').some(([, scope]) => scope.startsWith("comment"))).toBe(
      false,
    );
  });

  test("a raw literal spans lines, with no escapes and no comments in it (grammar.md §1.6)", () => {
    const lines = ['let q = """', '    a\\n "b" // c', '    """;', "fn"];
    let stack = textmate.INITIAL;
    const all = [];
    for (const line of lines) {
      const { tokens, ruleStack } = grammar.tokenizeLine(line, stack);
      stack = ruleStack;
      all.push(
        tokens
          .map((token) => [line.slice(token.startIndex, token.endIndex), token.scopes.at(-1)])
          .filter(([text]) => text.trim().length > 0),
      );
    }
    expect(all[1].every(([, scope]) => scope === "string.quoted.triple.ceesafe")).toBe(true);
    expect(all[2][0]).toEqual(['    """', "string.quoted.triple.ceesafe"]);
    expect(all[3]).toEqual([["fn", "keyword.declaration.ceesafe"]]);
  });

  test("comments", () => {
    expect(scopeOf("/// Docs.", "/// Docs.")).toBe("comment.line.documentation.ceesafe");
    expect(scopeOf("x(); // later", "// later")).toBe("comment.line.double-slash.ceesafe");
    expect(scopes("/* a */ fn").at(-1)).toEqual(["fn", "keyword.declaration.ceesafe"]);
  });
});
