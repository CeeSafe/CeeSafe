// `ceesafec lsp` (spec/tooling.md §2) driven by vscode-jsonrpc, the library under VS Code's language client: the
// server's framing and messages must work with it. Needs a built ceesafec: CEESAFEC, or the repository's out/ceesafec.
import { spawn } from "node:child_process";
import { existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { afterAll, beforeAll, describe, expect, test } from "vitest";
import rpc from "vscode-jsonrpc/node.js";

const ceesafec =
  process.env.CEESAFEC ?? fileURLToPath(new URL("../../../out/ceesafec", import.meta.url));
const uri = "file:///work/demo.cee";
const text =
  "fn length(a: i32) -> i32 => a + 1;\n\nfn main() {\n    let n = length(2);\n    let b: bool = n;\n}\n";

describe.skipIf(!existsSync(ceesafec))("ceesafec lsp through vscode-jsonrpc", () => {
  let server;
  let connection;
  const published = [];

  beforeAll(async () => {
    server = spawn(ceesafec, ["lsp"], { stdio: ["pipe", "pipe", "inherit"] });
    connection = rpc.createMessageConnection(
      new rpc.StreamMessageReader(server.stdout),
      new rpc.StreamMessageWriter(server.stdin),
    );
    connection.onNotification("textDocument/publishDiagnostics", (params) =>
      published.push(params),
    );
    connection.listen();
    await connection.sendRequest("initialize", {
      processId: null,
      rootUri: null,
      capabilities: {},
    });
    connection.sendNotification("initialized", {});
    connection.sendNotification("textDocument/didOpen", {
      textDocument: { uri, languageId: "ceesafe", version: 1, text },
    });
  });

  afterAll(async () => {
    await connection.sendRequest("shutdown");
    const exited = new Promise((resolve) => server.on("exit", resolve));
    connection.sendNotification("exit");
    expect(await exited).toBe(0);
    connection.dispose();
  });

  test("publishes the document's diagnostics", async () => {
    await expect.poll(() => published.length).toBe(1);
    expect(published[0].uri).toBe(uri);
    expect(published[0].diagnostics.map((d) => [d.code, d.range.start.line])).toEqual([
      ["N0205", 4],
    ]);
  });

  test("answers hover and definition", async () => {
    const hover = await connection.sendRequest("textDocument/hover", {
      textDocument: { uri },
      position: { line: 3, character: 8 },
    });
    expect(hover.contents.value).toBe("```ceesafe\nlet n: i32\n```");
    const definition = await connection.sendRequest("textDocument/definition", {
      textDocument: { uri },
      position: { line: 3, character: 13 },
    });
    expect(definition.range.start).toEqual({ line: 0, character: 3 });
  });

  test("an unknown method is MethodNotFound", async () => {
    await expect(connection.sendRequest("textDocument/rename", {})).rejects.toMatchObject({
      code: -32601,
    });
  });
});
