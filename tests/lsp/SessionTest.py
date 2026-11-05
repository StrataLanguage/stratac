#!/usr/bin/env python3
"""Drives strata-lsp over stdio and checks the messages it sends back.

usage: SessionTest.py <path-to-strata-lsp>
"""

import json
import os
import subprocess
import sys
import tempfile
from urllib.parse import quote

BAD_ASSIGN = 'int main()\n{\n    int x = "hi";\n    return x;\n}\n'


class Client:
    def __init__(self, exe, args):
        self.proc = subprocess.Popen(
            [exe, *args],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.pending = []
        self.next_id = 1

    def send_raw(self, body):
        self.proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.proc.stdin.flush()

    def send(self, msg):
        self.send_raw(json.dumps(msg).encode("utf-8"))

    def request(self, method, params=None):
        rid = self.next_id
        self.next_id += 1
        msg = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        self.send(msg)
        return rid

    def notify(self, method, params=None):
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        self.send(msg)

    def read(self):
        length = None
        while True:
            line = self.proc.stdout.readline()
            if line == b"":
                stderr = self.proc.stderr.read().decode("utf-8", "replace")
                raise RuntimeError("server closed stdout. stderr: " + stderr)
            line = line.strip()
            if line == b"":
                break
            if line.lower().startswith(b"content-length:"):
                length = int(line.split(b":", 1)[1])
        body = self.proc.stdout.read(length)
        return json.loads(body)

    def wait(self, predicate):
        for i, msg in enumerate(self.pending):
            if predicate(msg):
                return self.pending.pop(i)
        while True:
            msg = self.read()
            if predicate(msg):
                return msg
            self.pending.append(msg)

    def response(self, rid):
        return self.wait(lambda m: m.get("id") == rid and "method" not in m)

    def diagnostics(self, uri):
        msg = self.wait(
            lambda m: m.get("method") == "textDocument/publishDiagnostics"
            and m["params"]["uri"] == uri
        )
        return msg["params"]["diagnostics"]


def file_uri(path):
    return "file://" + quote(path)


def check(cond, what):
    if not cond:
        raise AssertionError(what)
    print("ok  " + what)


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def open_doc(client, uri, text):
    client.notify(
        "textDocument/didOpen",
        {"textDocument": {"uri": uri, "languageId": "strata", "version": 1, "text": text}},
    )


def change_doc(client, uri, text):
    client.notify(
        "textDocument/didChange",
        {"textDocument": {"uri": uri, "version": 2}, "contentChanges": [{"text": text}]},
    )


def published_before_barrier(client):
    """Returns (uri, diagnostics) for every publish the server sent before answering a barrier request."""
    rid = client.request("strata/barrier", {})
    msgs = []
    while True:
        msg = client.pending.pop(0) if client.pending else client.read()
        if msg.get("id") == rid and "method" not in msg:
            break
        msgs.append(msg)
    return [
        (m["params"]["uri"], m["params"]["diagnostics"])
        for m in msgs
        if m.get("method") == "textDocument/publishDiagnostics"
    ]


def run(exe):
    root = tempfile.mkdtemp(prefix="strata lsp ")  # a space exercises percent-encoding
    lib_dir = os.path.join(root, "lib")
    main_dir = os.path.join(root, "main")
    write(os.path.join(lib_dir, "shared.strata"), "int shared_value()\n{\n    return 7;\n}\n")
    write(os.path.join(lib_dir, "broken.strata"), BAD_ASSIGN.replace("main", "broken"))

    client = Client(exe, ["-I", lib_dir])

    # Handshake.
    rid = client.request("initialize", {"processId": None, "rootUri": None, "capabilities": {}})
    init = client.response(rid)["result"]
    check(init["capabilities"]["textDocumentSync"]["change"] == 1, "initialize advertises full-text sync")
    check(init["serverInfo"]["name"] == "strata-lsp", "initialize names the server")
    client.notify("initialized", {})

    # An error is published at the right line and column.
    bad_uri = file_uri(os.path.join(main_dir, "bad.strata"))
    open_doc(client, bad_uri, BAD_ASSIGN)
    diags = client.diagnostics(bad_uri)
    check(len(diags) == 1, "didOpen with a type error publishes one diagnostic")
    d = diags[0]
    check(d["range"]["start"] == {"line": 2, "character": 4}, "diagnostic starts at line 2, column 4 (0-based)")
    check(d["severity"] == 1, "type error has error severity")
    check("cannot be initialized" in d["message"], "diagnostic carries the compiler's message")

    # Fixing the buffer clears the diagnostic.
    fixed = 'int main()\n{\n    int x = 3;\n    return x;\n}\n'
    change_doc(client, bad_uri, fixed)
    check(client.diagnostics(bad_uri) == [], "didChange to valid code clears the diagnostic")

    # Columns count UTF-16 code units, so a multi-byte character before the error shifts it by one.
    uni_uri = file_uri(os.path.join(main_dir, "uni.strata"))
    uni_text = 'int main()\n{\n    /* é */ int x = "hi";\n    return 0;\n}\n'
    open_doc(client, uni_uri, uni_text)
    diags = client.diagnostics(uni_uri)
    line = uni_text.split("\n")[2]
    prefix = line[: line.index("int x")]
    expected = len(prefix.encode("utf-16-le")) // 2
    check(len(diags) == 1, "error after a non-ASCII comment is reported")
    check(
        diags[0]["range"]["start"] == {"line": 2, "character": expected},
        "column after a non-ASCII character counts UTF-16 units",
    )

    # A valid module that imports a file found through -I.
    ok_uri = file_uri(os.path.join(main_dir, "uses_shared.strata"))
    open_doc(client, ok_uri, "import shared;\nint main()\n{\n    return shared_value();\n}\n")
    check(client.diagnostics(ok_uri) == [], "import resolved through -I produces no diagnostics")

    # An error inside an imported file is published under that file's own URI.
    imp_uri = file_uri(os.path.join(main_dir, "imports_broken.strata"))
    open_doc(client, imp_uri, "import broken;\nint main()\n{\n    return 0;\n}\n")
    check(client.diagnostics(imp_uri) == [], "the importing file itself stays clean")
    broken_uri = file_uri(os.path.realpath(os.path.join(lib_dir, "broken.strata")))
    diags = client.diagnostics(broken_uri)
    check(len(diags) == 1, "an error in an imported file is published under that file")
    check("cannot be initialized" in diags[0]["message"], "imported-file diagnostic has the compiler's message")

    # A missing import is reported on the importing file.
    missing_uri = file_uri(os.path.join(main_dir, "missing.strata"))
    open_doc(client, missing_uri, "import nothere;\nint main()\n{\n    return 0;\n}\n")
    diags = client.diagnostics(missing_uri)
    check(
        len(diags) == 1 and "nothere" in diags[0]["message"] and diags[0]["severity"] == 1,
        "a missing import is reported as an error",
    )

    # Closing a document clears what it published.
    client.notify("textDocument/didClose", {"textDocument": {"uri": imp_uri}})
    check(client.diagnostics(imp_uri) == [], "didClose clears the document's diagnostics")
    check(client.diagnostics(broken_uri) == [], "didClose clears diagnostics published for imported files")

    # An import cycle back to the open file reuses its buffer instead of loading the file a second time.
    write(os.path.join(main_dir, "cyc_b.strata"), "import cyc_a;\nint fb()\n{\n    return 1;\n}\n")
    cyc_a_text = "import cyc_b;\nint fa()\n{\n    return fb();\n}\n"
    cyc_a = os.path.join(main_dir, "cyc_a.strata")
    write(cyc_a, cyc_a_text)
    open_doc(client, file_uri(cyc_a), cyc_a_text + "int fa2()\n{\n    return 2;\n}\n")
    check(client.diagnostics(file_uri(cyc_a)) == [], "an import cycle to the open file reports no duplicates")

    # Imports of an open file use its unsaved buffer. On disk, dep.strata has a type error.
    dep = os.path.join(main_dir, "dep.strata")
    write(dep, 'int dep_value()\n{\n    int x = "hi";\n    return x;\n}\n')
    dep_uri = file_uri(dep)
    open_doc(client, dep_uri, "int dep_value()\n{\n    return 1;\n}\n")
    check(client.diagnostics(dep_uri) == [], "the open dependency's fixed buffer is clean")

    user_uri = file_uri(os.path.join(main_dir, "dep_user.strata"))
    open_doc(client, user_uri, "import dep;\nint main()\n{\n    return dep_value();\n}\n")
    published = published_before_barrier(client)
    check(
        any(uri == user_uri and diags == [] for uri, diags in published),
        "an import of an open file is analyzed from its buffer",
    )
    check(
        not any(uri.endswith("/dep.strata") for uri, _ in published),
        "an importer does not publish over an open file's own diagnostics",
    )

    # Editing the open dependency re-analyzes the open importer.
    change_doc(client, dep_uri, "int dep_renamed()\n{\n    return 1;\n}\n")
    check(client.diagnostics(dep_uri) == [], "the renamed dependency is itself clean")
    check(len(client.diagnostics(user_uri)) >= 1, "the importer is re-analyzed when its dependency changes")
    change_doc(client, dep_uri, "int dep_value()\n{\n    return 1;\n}\n")
    client.diagnostics(dep_uri)
    check(client.diagnostics(user_uri) == [], "the importer clears once the dependency is restored")

    # Closing the dependency sends the importer back to the copy on disk, which has the type error.
    client.notify("textDocument/didClose", {"textDocument": {"uri": dep_uri}})
    check(client.diagnostics(dep_uri) == [], "didClose clears the dependency's own diagnostics")
    disk_dep_uri = file_uri(os.path.realpath(dep))
    check(
        len(client.diagnostics(disk_dep_uri)) == 1,
        "after the dependency closes, its importer reports the error on disk",
    )

    # Closing an importer leaves an open dependency's diagnostics alone.
    open_doc(client, dep_uri, 'int dep_value()\n{\n    int y = "no";\n    return 1;\n}\n')
    check(len(client.diagnostics(dep_uri)) == 1, "the reopened dependency reports its own error")
    client.notify("textDocument/didClose", {"textDocument": {"uri": user_uri}})
    published = published_before_barrier(client)
    check(
        not any(uri.endswith("/dep.strata") for uri, _ in published),
        "closing an importer does not clear an open file's diagnostics",
    )
    client.notify("textDocument/didClose", {"textDocument": {"uri": dep_uri}})

    # Unknown requests get an error response with the same id.
    rid = client.request("textDocument/nope", {})
    reply = client.response(rid)
    check(reply["error"]["code"] == -32601, "unknown request gets method-not-found")

    # Malformed JSON gets a parse error, and the server keeps running.
    client.send_raw(b"{not json")
    reply = client.wait(lambda m: m.get("error", {}).get("code") == -32700)
    check(reply["id"] is None, "malformed JSON gets a parse error with a null id")
    rid = client.request("initialize", {})
    check("result" in client.response(rid), "server still answers after a parse error")

    # Shutdown then exit ends the process with status 0.
    rid = client.request("shutdown")
    check(client.response(rid)["result"] is None, "shutdown returns null")
    client.notify("exit")
    try:
        status = client.proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        raise AssertionError("server did not exit after exit notification")
    check(status == 0, "exit after shutdown returns status 0")


def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        run(sys.argv[1])
    except AssertionError as e:
        print("FAIL: " + str(e), file=sys.stderr)
        return 1
    print("lsp session: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
