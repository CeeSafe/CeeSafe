// The compiler's version, read from the compiler's source (Decision 0011 §6: the title page's version is written by
// the build, not by hand). A page writes `%ceesafec-version%` where the version goes: the site (.vitepress/config.mts)
// and the machine-readable book (scripts/prepare.mjs) both put the version there.
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const source = join(dirname(fileURLToPath(import.meta.url)), "..", "src", "backend", "c_program.cee");
const match = readFileSync(source, "utf8").match(/fn ceesafeVersion\(\) -> StringView => "([0-9]+\.[0-9]+\.[0-9]+[^"]*)";/);
if (!match) {
  throw new Error("book/version.mjs: src/backend/c_program.cee has no ceesafeVersion() to read the version from");
}

/** The version of the compiler in this tree, "0.42.0". */
export const ceesafeVersion = match[1];

/** The text a page writes for the version. */
export const versionToken = "%ceesafec-version%";

/** A page's text with the version in the token's place. */
export const withVersion = (text) => text.replaceAll(versionToken, ceesafeVersion);
