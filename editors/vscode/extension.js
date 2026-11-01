// The CeeSafe extension's client (spec/tooling.md §2.4): starts `ceesafec lsp` for .cee documents.
const vscode = require("vscode");
const { LanguageClient } = require("vscode-languageclient/node");

let client;

function activate(context) {
  const command = vscode.workspace.getConfiguration("ceesafe").get("server.path", "ceesafec");
  const server = { command, args: ["lsp"] };
  client = new LanguageClient(
    "ceesafe",
    "CeeSafe",
    { run: server, debug: server },
    {
      documentSelector: [
        { scheme: "file", language: "ceesafe" },
        { scheme: "untitled", language: "ceesafe" },
      ],
    },
  );
  context.subscriptions.push(client);
  return client.start();
}

function deactivate() {
  return client ? client.stop() : undefined;
}

module.exports = { activate, deactivate };
