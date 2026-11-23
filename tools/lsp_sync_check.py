#!/usr/bin/env python3
"""The language server keeps open documents' diagnostics in step with the package (spec/tooling.md §2.1, §2.2):

- capabilities: full-text synchronisation with `save`;
- watches: a client that registers file watchers on request is asked to watch the package's sources, manifests and
  locks once it is initialised, and one that does not is asked nothing;
- watched files: after another file of the package changes on disk, `workspace/didChangeWatchedFiles` republishes the
  open document's diagnostics (here an N0200, a function the other file no longer has);
- open texts: a check reads the package's other open files as the editor holds them, so opening the other file with
  text that has the function again, then saving, clears the diagnostic while the file on disk is still without it;
- save: `textDocument/didSave` checks the open documents again (the same step).

A session is interactive: the files change between messages, and each step waits for the diagnostics it expects.

usage: python3 tools/lsp_sync_check.py --compiler out/ceesafec
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
import threading

MANIFEST = '[package]\nname = "sync"\nversion = "0.1.0"\nkind = "executable"\n'
MAIN = "fn main() {\n    Console.writeLine(helper());\n}\n"
HELPER = "fn helper() -> i32 => 42;\n"
RENAMED = "fn renamed() -> i32 => 42;\n"


class Session:
    def __init__(self, compiler, directory):
        self.process = subprocess.Popen([compiler, "lsp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.DEVNULL, cwd=directory)
        self.next = 1

    def send(self, method, params, request=False):
        message = {"jsonrpc": "2.0", "method": method, "params": params}
        if request:
            message["id"] = self.next
            self.next += 1
        body = json.dumps(message).encode("utf-8")
        self.process.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.process.stdin.flush()
        return message.get("id")

    def read(self):
        """The next message from the server, or None at the end of its output."""
        length = None
        while True:
            line = self.process.stdout.readline()
            if not line:
                return None
            line = line.strip()
            if not line:
                break
            if line.lower().startswith(b"content-length:"):
                length = int(line.split(b":")[1])
        return json.loads(self.process.stdout.read(length))

    def until(self, wanted, seconds=120):
        """Messages up to the first for which `wanted` is true; that one, or None when none comes in time."""
        found = []

        def run():
            while True:
                message = self.read()
                if message is None:
                    return
                if wanted(message):
                    found.append(message)
                    return

        thread = threading.Thread(target=run, daemon=True)
        thread.start()
        thread.join(seconds)
        return found[0] if found else None

    def close(self):
        try:
            self.send("shutdown", None, request=True)
            self.send("exit", None)
            self.process.stdin.close()
            self.process.wait(timeout=30)
        except Exception:
            self.process.kill()


def uri(path):
    return "file://" + path


def published(target):
    return lambda m: m.get("method") == "textDocument/publishDiagnostics" and m["params"]["uri"] == target


def codes(message):
    return sorted(d.get("code") for d in message["params"]["diagnostics"]) if message else None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    problems = []
    with tempfile.TemporaryDirectory() as work:
        work = os.path.realpath(work)
        os.mkdir(os.path.join(work, "src"))
        main_path = os.path.join(work, "src", "main.cee")
        helper_path = os.path.join(work, "src", "helper.cee")
        for path, text in ((os.path.join(work, "ceesafe.toml"), MANIFEST), (main_path, MAIN), (helper_path, HELPER)):
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)

        session = Session(compiler, work)
        watching = {"workspace": {"didChangeWatchedFiles": {"dynamicRegistration": True}}}
        request = session.send("initialize", {"processId": None, "rootUri": uri(work), "capabilities": watching},
                               request=True)
        answer = session.until(lambda m: m.get("id") == request)
        sync = (answer or {}).get("result", {}).get("capabilities", {}).get("textDocumentSync")
        if not isinstance(sync, dict) or sync.get("change") != 1 or not sync.get("save") or not sync.get("openClose"):
            problems.append(f"capabilities: textDocumentSync is {sync}, expected openClose, change 1 and save")
        session.send("initialized", {})
        asked = session.until(lambda m: m.get("method") == "client/registerCapability", 30)
        watchers = []
        if asked:
            for registration in asked["params"]["registrations"]:
                if registration["method"] == "workspace/didChangeWatchedFiles":
                    watchers = [w["globPattern"] for w in registration["registerOptions"]["watchers"]]
        if sorted(watchers) != ["**/*.cee", "**/ceesafe.lock", "**/ceesafe.toml"]:
            problems.append(f"watches: the server asked to watch {watchers}")
        if asked:
            body = json.dumps({"jsonrpc": "2.0", "id": asked["id"], "result": None}).encode("utf-8")
            session.process.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
            session.process.stdin.flush()

        session.send("textDocument/didOpen", {"textDocument": {"uri": uri(main_path), "languageId": "ceesafe",
                                                               "version": 1, "text": MAIN}})
        first = session.until(published(uri(main_path)))
        if codes(first) != []:
            problems.append(f"open: expected no diagnostics for main.cee, got {codes(first)}")

        with open(helper_path, "w", encoding="utf-8") as f:
            f.write(RENAMED)
        session.send("workspace/didChangeWatchedFiles", {"changes": [{"uri": uri(helper_path), "type": 2}]})
        stale = session.until(published(uri(main_path)))
        if codes(stale) != ["N0200"]:
            problems.append(f"watched files: expected N0200 in main.cee after helper.cee changed on disk, got "
                            f"{codes(stale)}")

        session.send("textDocument/didOpen", {"textDocument": {"uri": uri(helper_path), "languageId": "ceesafe",
                                                               "version": 1, "text": HELPER}})
        if session.until(published(uri(helper_path))) is None:
            problems.append("open texts: no diagnostics published for helper.cee")
        session.send("textDocument/didSave", {"textDocument": {"uri": uri(main_path)}})
        fresh = session.until(published(uri(main_path)))
        if codes(fresh) != []:
            problems.append(f"open texts and save: expected main.cee clean with helper.cee's open text, got "
                            f"{codes(fresh)}")
        with open(helper_path, encoding="utf-8") as f:
            if f.read() != RENAMED:
                problems.append("open texts: the file on disk changed")
        session.close()

        quiet = Session(compiler, work)
        request = quiet.send("initialize", {"processId": None, "rootUri": uri(work), "capabilities": {}},
                             request=True)
        quiet.until(lambda m: m.get("id") == request)
        quiet.send("initialized", {})
        ping = quiet.send("shutdown", None, request=True)
        seen = []
        quiet.until(lambda m: seen.append(m.get("method")) or m.get("id") == ping, 60)
        if "client/registerCapability" in seen:
            problems.append("watches: a client without dynamic registration was asked to register watchers")
        quiet.close()

    for p in problems:
        print("FAIL", p)
    print(f"lsp_sync_check: 6 checks: {len(problems)} problems")
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
