#!/usr/bin/env python3
"""LSP server integration tests for rsg-lsp.

Tests the Language Server Protocol implementation by sending JSON-RPC
messages over stdin/stdout to the rsg-lsp binary and validating responses.

Usage:
    python3 tests/lsp/test_lsp.py [--lsp=<path>] [--std-path=<dir>]

Exit code 0 on success, 1 on failure.  Output is TAP v13.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

IS_WINDOWS = platform.system() == "Windows"
EXE_EXT = ".exe" if IS_WINDOWS else ""

# ── JSON-RPC transport ──────────────────────────────────────────────────


def encode_message(obj: dict) -> bytes:
    """Encode a JSON-RPC message with Content-Length header."""
    body = json.dumps(obj).encode("utf-8")
    header = f"Content-Length: {len(body)}\r\n\r\n".encode("ascii")
    return header + body


def read_message(proc: subprocess.Popen, timeout: float = 10.0) -> dict | None:
    """Read one JSON-RPC message from the process stdout (thread-safe timeout)."""
    result_holder: list[bytes] = []
    error_holder: list[bool] = [False]

    def _read_header():
        """Read bytes until \\r\\n\\r\\n or EOF."""
        buf = b""
        try:
            while True:
                ch = proc.stdout.read(1)
                if ch == b"":
                    error_holder[0] = True
                    return
                buf += ch
                if buf.endswith(b"\r\n\r\n"):
                    result_holder.append(buf)
                    return
        except Exception:
            error_holder[0] = True

    t = threading.Thread(target=_read_header, daemon=True)
    t.start()
    t.join(timeout=timeout)
    if t.is_alive() or error_holder[0] or not result_holder:
        return None

    header_bytes = result_holder[0]
    header_str = header_bytes.decode("ascii", errors="replace")
    content_length = None
    for line in header_str.split("\r\n"):
        if line.lower().startswith("content-length:"):
            content_length = int(line.split(":", 1)[1].strip())
            break
    if content_length is None:
        return None

    # Read body with thread timeout.
    body_holder: list[bytes] = []

    def _read_body():
        try:
            body_holder.append(proc.stdout.read(content_length))
        except Exception:
            pass

    bt = threading.Thread(target=_read_body, daemon=True)
    bt.start()
    bt.join(timeout=timeout)
    if bt.is_alive() or not body_holder or len(body_holder[0]) != content_length:
        return None

    return json.loads(body_holder[0])


def send(proc: subprocess.Popen, obj: dict) -> None:
    """Send a JSON-RPC message to the process."""
    proc.stdin.write(encode_message(obj))
    proc.stdin.flush()


def request(proc: subprocess.Popen, id: int, method: str, params: dict | None = None) -> None:
    """Send a JSON-RPC request."""
    msg: dict = {"jsonrpc": "2.0", "id": id, "method": method}
    if params is not None:
        msg["params"] = params
    send(proc, msg)


def notification(proc: subprocess.Popen, method: str, params: dict | None = None) -> None:
    """Send a JSON-RPC notification (no id)."""
    msg: dict = {"jsonrpc": "2.0", "method": method}
    if params is not None:
        msg["params"] = params
    send(proc, msg)


# ── Helpers ─────────────────────────────────────────────────────────────


def file_uri(path: str) -> str:
    """Convert a filesystem path to a file:// URI."""
    p = Path(path).resolve().as_posix()
    if not p.startswith("/"):
        p = "/" + p
    return "file://" + p


def read_all_messages(proc: subprocess.Popen, timeout: float = 3.0) -> list[dict]:
    """Read all available messages until timeout with no new message."""
    messages = []
    while True:
        msg = read_message(proc, timeout=timeout)
        if msg is None:
            break
        messages.append(msg)
    return messages


# ── Test runner ─────────────────────────────────────────────────────────


class LspTest:
    """Simple TAP-based LSP test runner."""

    def __init__(self, lsp_path: str, std_path: str | None):
        self.lsp_path = lsp_path
        self.std_path = std_path
        self.passed = 0
        self.failed = 0
        self.test_num = 0
        self.total_tests = 0

    def start(self, total: int) -> None:
        self.total_tests = total
        print(f"TAP version 13\n1..{total}")

    def ok(self, desc: str) -> None:
        self.test_num += 1
        self.passed += 1
        print(f"ok {self.test_num} - {desc}")

    def fail(self, desc: str, detail: str = "") -> None:
        self.test_num += 1
        self.failed += 1
        print(f"not ok {self.test_num} - {desc}")
        if detail:
            for line in detail.split("\n"):
                print(f"  # {line}")

    def start_server(self) -> subprocess.Popen:
        """Start the LSP server process."""
        cmd = [self.lsp_path]
        if self.std_path:
            cmd.append(f"--std-path={self.std_path}")
        return subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    def initialize(self, proc: subprocess.Popen) -> dict | None:
        """Perform the initialize/initialized handshake. Returns the init result."""
        request(proc, 1, "initialize", {
            "processId": os.getpid(),
            "rootUri": file_uri(os.getcwd()),
            "capabilities": {},
        })
        resp = read_message(proc)
        if resp is not None:
            notification(proc, "initialized", {})
        return resp

    def summary(self) -> int:
        """Print summary and return exit code."""
        total = self.passed + self.failed
        print(f"# {self.passed}/{total} tests passed", file=sys.stderr)
        return 0 if self.failed == 0 else 1


# ── Individual tests ────────────────────────────────────────────────────


def test_initialize(t: LspTest) -> None:
    """Server responds to initialize with capabilities."""
    proc = t.start_server()
    try:
        resp = t.initialize(proc)
        if resp is None:
            t.fail("initialize: no response")
            return
        if "result" not in resp:
            t.fail("initialize: no result field", json.dumps(resp, indent=2))
            return
        caps = resp["result"].get("capabilities", {})
        # Must declare textDocumentSync capability.
        if "textDocumentSync" not in caps:
            t.fail("initialize: missing textDocumentSync", json.dumps(caps, indent=2))
            return
        t.ok("initialize: responds with capabilities")
    finally:
        proc.kill()
        proc.wait()


def test_shutdown_exit(t: LspTest) -> None:
    """Server shuts down and exits cleanly."""
    proc = t.start_server()
    try:
        t.initialize(proc)
        # shutdown
        request(proc, 2, "shutdown")
        resp = read_message(proc)
        if resp is None or resp.get("id") != 2:
            t.fail("shutdown: no valid response")
            return
        # exit
        notification(proc, "exit")
        proc.wait(timeout=5)
        if proc.returncode != 0:
            t.fail(f"exit: non-zero return code {proc.returncode}")
            return
        t.ok("shutdown/exit: clean termination")
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        t.fail("exit: server did not terminate")


def test_didopen_clean_file(t: LspTest) -> None:
    """Opening a clean file produces empty diagnostics."""
    proc = t.start_server()
    try:
        t.initialize(proc)
        # Write a valid .rsg file that uses no builtins (LSP runs without prelude).
        clean_src = "fn main() {\n    x := 42\n}\n"
        with tempfile.NamedTemporaryFile(
            suffix=".rsg", mode="w", delete=False, encoding="utf-8"
        ) as f:
            f.write(clean_src)
            tmp_path = f.name
        try:
            uri = file_uri(tmp_path)
            notification(proc, "textDocument/didOpen", {
                "textDocument": {
                    "uri": uri,
                    "languageId": "resurg",
                    "version": 1,
                    "text": clean_src,
                },
            })
            # Read diagnostics notification.
            msgs = read_all_messages(proc, timeout=3.0)
            diag_msgs = [m for m in msgs if m.get("method") == "textDocument/publishDiagnostics"]
            if len(diag_msgs) == 0:
                t.fail("didOpen clean: no publishDiagnostics")
                return
            diags = diag_msgs[-1]["params"]["diagnostics"]
            if len(diags) != 0:
                t.fail("didOpen clean: expected empty diagnostics", json.dumps(diags, indent=2))
                return
            t.ok("didOpen clean: empty diagnostics published")
        finally:
            os.unlink(tmp_path)
    finally:
        proc.kill()
        proc.wait()


def test_didopen_error_file(t: LspTest) -> None:
    """Opening a file with errors produces diagnostics with source spans."""
    proc = t.start_server()
    try:
        t.initialize(proc)
        error_src = "fn main() {\n    x := unknown_var + 1\n}\n"
        with tempfile.NamedTemporaryFile(
            suffix=".rsg", mode="w", delete=False, encoding="utf-8"
        ) as f:
            f.write(error_src)
            tmp_path = f.name
        try:
            uri = file_uri(tmp_path)
            notification(proc, "textDocument/didOpen", {
                "textDocument": {
                    "uri": uri,
                    "languageId": "resurg",
                    "version": 1,
                    "text": error_src,
                },
            })
            msgs = read_all_messages(proc, timeout=5.0)
            diag_msgs = [m for m in msgs if m.get("method") == "textDocument/publishDiagnostics"]
            if len(diag_msgs) == 0:
                t.fail("didOpen error: no publishDiagnostics")
                return
            diags = diag_msgs[-1]["params"]["diagnostics"]
            if len(diags) == 0:
                t.fail("didOpen error: expected non-empty diagnostics")
                return
            # Verify diagnostics have required LSP fields.
            d = diags[0]
            if "range" not in d:
                t.fail("didOpen error: diagnostic missing 'range'", json.dumps(d, indent=2))
                return
            if "message" not in d:
                t.fail("didOpen error: diagnostic missing 'message'", json.dumps(d, indent=2))
                return
            rng = d["range"]
            if "start" not in rng or "end" not in rng:
                t.fail("didOpen error: range missing start/end", json.dumps(rng, indent=2))
                return
            if "line" not in rng["start"] or "character" not in rng["start"]:
                t.fail("didOpen error: start missing line/character")
                return
            t.ok("didOpen error: diagnostics with source spans")
        finally:
            os.unlink(tmp_path)
    finally:
        proc.kill()
        proc.wait()


def test_didchange(t: LspTest) -> None:
    """Changing file content re-publishes diagnostics."""
    proc = t.start_server()
    try:
        t.initialize(proc)
        # Start with error code.
        error_src = "fn main() {\n    x := bad_var\n}\n"
        with tempfile.NamedTemporaryFile(
            suffix=".rsg", mode="w", delete=False, encoding="utf-8"
        ) as f:
            f.write(error_src)
            tmp_path = f.name
        try:
            uri = file_uri(tmp_path)
            notification(proc, "textDocument/didOpen", {
                "textDocument": {
                    "uri": uri,
                    "languageId": "resurg",
                    "version": 1,
                    "text": error_src,
                },
            })
            # Read exactly the initial publishDiagnostics.
            initial = read_message(proc, timeout=5.0)
            if initial is None or initial.get("method") != "textDocument/publishDiagnostics":
                t.fail("didChange: no initial publishDiagnostics")
                return

            # Fix the file (no builtins — LSP runs without prelude).
            fixed_src = "fn main() {\n    x := 42\n}\n"
            notification(proc, "textDocument/didChange", {
                "textDocument": {"uri": uri, "version": 2},
                "contentChanges": [{"text": fixed_src}],
            })
            # Read the updated publishDiagnostics.
            updated = read_message(proc, timeout=5.0)
            if updated is None:
                t.fail("didChange: no publishDiagnostics after change")
                return
            if updated.get("method") != "textDocument/publishDiagnostics":
                t.fail("didChange: expected publishDiagnostics, got " + updated.get("method", ""))
                return
            diags = updated["params"]["diagnostics"]
            if len(diags) != 0:
                t.fail("didChange: expected empty diagnostics after fix", json.dumps(diags, indent=2))
                return
            t.ok("didChange: diagnostics updated after content change")
        finally:
            os.unlink(tmp_path)
    finally:
        proc.kill()
        proc.wait()


def test_diagnostics_severity(t: LspTest) -> None:
    """Diagnostics include LSP-compatible severity values."""
    proc = t.start_server()
    try:
        t.initialize(proc)
        error_src = "fn main() {\n    var x: i32 = true\n}\n"
        with tempfile.NamedTemporaryFile(
            suffix=".rsg", mode="w", delete=False, encoding="utf-8"
        ) as f:
            f.write(error_src)
            tmp_path = f.name
        try:
            uri = file_uri(tmp_path)
            notification(proc, "textDocument/didOpen", {
                "textDocument": {
                    "uri": uri,
                    "languageId": "resurg",
                    "version": 1,
                    "text": error_src,
                },
            })
            msgs = read_all_messages(proc, timeout=5.0)
            diag_msgs = [m for m in msgs if m.get("method") == "textDocument/publishDiagnostics"]
            if len(diag_msgs) == 0:
                t.fail("diagnostics severity: no publishDiagnostics")
                return
            diags = diag_msgs[-1]["params"]["diagnostics"]
            if len(diags) == 0:
                t.fail("diagnostics severity: expected non-empty diagnostics")
                return
            d = diags[0]
            severity = d.get("severity")
            if severity is None:
                t.fail("diagnostics severity: missing severity field")
                return
            # LSP severity: 1=Error, 2=Warning, 3=Information, 4=Hint
            if severity not in (1, 2, 3, 4):
                t.fail(f"diagnostics severity: invalid value {severity}")
                return
            if severity != 1:
                t.fail(f"diagnostics severity: expected 1 (Error), got {severity}")
                return
            t.ok("diagnostics severity: LSP-compatible severity")
        finally:
            os.unlink(tmp_path)
    finally:
        proc.kill()
        proc.wait()


def test_diagnostics_message_content(t: LspTest) -> None:
    """Diagnostic messages describe the actual problem."""
    proc = t.start_server()
    try:
        t.initialize(proc)
        error_src = "fn main() {\n    x := unknown_var\n}\n"
        with tempfile.NamedTemporaryFile(
            suffix=".rsg", mode="w", delete=False, encoding="utf-8"
        ) as f:
            f.write(error_src)
            tmp_path = f.name
        try:
            uri = file_uri(tmp_path)
            notification(proc, "textDocument/didOpen", {
                "textDocument": {
                    "uri": uri,
                    "languageId": "resurg",
                    "version": 1,
                    "text": error_src,
                },
            })
            msgs = read_all_messages(proc, timeout=5.0)
            diag_msgs = [m for m in msgs if m.get("method") == "textDocument/publishDiagnostics"]
            if len(diag_msgs) == 0:
                t.fail("diagnostics message: no publishDiagnostics")
                return
            diags = diag_msgs[-1]["params"]["diagnostics"]
            if len(diags) == 0:
                t.fail("diagnostics message: expected non-empty diagnostics")
                return
            # At least one diagnostic should mention the problem.
            messages = [d["message"] for d in diags]
            found = any("unknown_var" in m for m in messages)
            if not found:
                t.fail("diagnostics message: none mentions 'unknown_var'",
                       "\n".join(messages))
                return
            t.ok("diagnostics message: describes the actual problem")
        finally:
            os.unlink(tmp_path)
    finally:
        proc.kill()
        proc.wait()


def test_hover(t: LspTest) -> None:
    """Hover on a function name returns signature and doc comment."""
    proc = t.start_server()
    try:
        t.initialize(proc)
        src = "/// Adds two numbers.\nfn add(a: i32, b: i32) -> i32 {\n    return a + b\n}\nfn main() {\n    x := add(1, 2)\n}\n"
        with tempfile.NamedTemporaryFile(
            suffix=".rsg", mode="w", delete=False, encoding="utf-8"
        ) as f:
            f.write(src)
            tmp_path = f.name
        try:
            uri = file_uri(tmp_path)
            notification(proc, "textDocument/didOpen", {
                "textDocument": {"uri": uri, "languageId": "resurg", "version": 1, "text": src},
            })
            # Consume the publishDiagnostics notification (one read, no orphan thread).
            _diag = read_message(proc, timeout=5.0)
            if _diag is None:
                t.fail("hover: no publishDiagnostics after didOpen")
                return

            # Hover over "add" on line 5 (0-based), character 9.
            request(proc, 10, "textDocument/hover", {
                "textDocument": {"uri": uri},
                "position": {"line": 5, "character": 9},
            })
            resp = read_message(proc, timeout=5.0)
            if resp is None or "result" not in resp:
                t.fail("hover: no response")
                return
            result = resp["result"]
            if result is None:
                t.fail("hover: result is null")
                return
            contents = result.get("contents", {})
            value = contents.get("value", "")
            if "add" not in value:
                t.fail("hover: fn name not in hover text", value)
                return
            if "Adds two numbers" not in value:
                t.fail("hover: doc comment not in hover text", value)
                return
            t.ok("hover: returns signature and doc comment")
        finally:
            os.unlink(tmp_path)
    finally:
        proc.kill()
        proc.wait()


def test_definition(t: LspTest) -> None:
    """Go-to-definition on a function name returns its location."""
    proc = t.start_server()
    try:
        t.initialize(proc)
        src = "fn greet() {\n    x := 1\n}\nfn main() {\n    greet()\n}\n"
        with tempfile.NamedTemporaryFile(
            suffix=".rsg", mode="w", delete=False, encoding="utf-8"
        ) as f:
            f.write(src)
            tmp_path = f.name
        try:
            uri = file_uri(tmp_path)
            notification(proc, "textDocument/didOpen", {
                "textDocument": {"uri": uri, "languageId": "resurg", "version": 1, "text": src},
            })
            _diag = read_message(proc, timeout=5.0)
            if _diag is None:
                t.fail("definition: no publishDiagnostics after didOpen")
                return

            # Go to definition of "greet" on line 4, character 5.
            request(proc, 11, "textDocument/definition", {
                "textDocument": {"uri": uri},
                "position": {"line": 4, "character": 5},
            })
            resp = read_message(proc, timeout=5.0)
            if resp is None or "result" not in resp:
                t.fail("definition: no response")
                return
            result = resp["result"]
            if result is None:
                t.fail("definition: result is null")
                return
            # Result should be a Location with uri and range.
            if "uri" not in result or "range" not in result:
                t.fail("definition: missing uri/range", json.dumps(result, indent=2))
                return
            start_line = result["range"]["start"]["line"]
            if start_line != 0:
                t.fail(f"definition: expected line 0, got {start_line}")
                return
            t.ok("definition: returns location of fn declaration")
        finally:
            os.unlink(tmp_path)
    finally:
        proc.kill()
        proc.wait()


def test_document_symbol(t: LspTest) -> None:
    """Document symbols lists functions and structs."""
    proc = t.start_server()
    try:
        t.initialize(proc)
        src = "struct Point {\n    x: f64\n    y: f64\n}\nfn distance() -> f64 {\n    return 0.0\n}\n"
        with tempfile.NamedTemporaryFile(
            suffix=".rsg", mode="w", delete=False, encoding="utf-8"
        ) as f:
            f.write(src)
            tmp_path = f.name
        try:
            uri = file_uri(tmp_path)
            notification(proc, "textDocument/didOpen", {
                "textDocument": {"uri": uri, "languageId": "resurg", "version": 1, "text": src},
            })
            _diag = read_message(proc, timeout=5.0)
            if _diag is None:
                t.fail("documentSymbol: no publishDiagnostics after didOpen")
                return

            request(proc, 12, "textDocument/documentSymbol", {
                "textDocument": {"uri": uri},
            })
            resp = read_message(proc, timeout=5.0)
            if resp is None or "result" not in resp:
                t.fail("documentSymbol: no response")
                return
            symbols = resp["result"]
            if not isinstance(symbols, list):
                t.fail("documentSymbol: result not a list", json.dumps(symbols, indent=2))
                return
            names = [s["name"] for s in symbols]
            if "Point" not in names:
                t.fail("documentSymbol: missing 'Point'", str(names))
                return
            if "distance" not in names:
                t.fail("documentSymbol: missing 'distance'", str(names))
                return
            # Check that Point has struct kind (23) and distance has function kind (12).
            point_sym = next(s for s in symbols if s["name"] == "Point")
            dist_sym = next(s for s in symbols if s["name"] == "distance")
            if point_sym.get("kind") != 23:
                t.fail(f"documentSymbol: Point kind={point_sym.get('kind')}, expected 23")
                return
            if dist_sym.get("kind") != 12:
                t.fail(f"documentSymbol: distance kind={dist_sym.get('kind')}, expected 12")
                return
            t.ok("documentSymbol: lists functions and structs with correct kinds")
        finally:
            os.unlink(tmp_path)
    finally:
        proc.kill()
        proc.wait()


# ── Main ────────────────────────────────────────────────────────────────


def test_hover_ext_value_recv(t: LspTest) -> None:
    """Hover on ext method with bare value receiver shows ext context and receiver."""
    proc = t.start_server()
    try:
        t.initialize(proc)
        # ext i32 method with bare value receiver (untyped alias)
        src = (
            "ext i32 {\n"
            "    /// Doubles an integer.\n"
            "    fn doubled(n) -> i32 {\n"
            "        return n * 2\n"
            "    }\n"
            "}\n"
            "fn main() {\n"
            "    x := 42.doubled()\n"
            "}\n"
        )
        with tempfile.NamedTemporaryFile(
            suffix=".rsg", mode="w", delete=False, encoding="utf-8"
        ) as f:
            f.write(src)
            tmp_path = f.name
        try:
            uri = file_uri(tmp_path)
            notification(proc, "textDocument/didOpen", {
                "textDocument": {"uri": uri, "languageId": "resurg", "version": 1, "text": src},
            })
            _diag = read_message(proc, timeout=5.0)
            if _diag is None:
                t.fail("hover ext value recv: no publishDiagnostics after didOpen")
                return

            # Hover over "doubled" on line 7, character 12.
            request(proc, 20, "textDocument/hover", {
                "textDocument": {"uri": uri},
                "position": {"line": 7, "character": 12},
            })
            resp = read_message(proc, timeout=5.0)
            if resp is None or "result" not in resp:
                t.fail("hover ext value recv: no response")
                return
            result = resp["result"]
            if result is None:
                t.fail("hover ext value recv: result is null")
                return
            value = result.get("contents", {}).get("value", "")
            if "ext i32" not in value:
                t.fail("hover ext value recv: missing 'ext i32' context", value)
                return
            if "doubled" not in value:
                t.fail("hover ext value recv: missing method name", value)
                return
            if "n /* i32 */" not in value:
                t.fail("hover ext value recv: missing receiver annotation 'n /* i32 */'", value)
                return
            if "Doubles an integer" not in value:
                t.fail("hover ext value recv: missing doc comment", value)
                return
            t.ok("hover ext value recv: shows ext context, receiver, and doc")
        finally:
            os.unlink(tmp_path)
    finally:
        proc.kill()
        proc.wait()


def test_hover_ext_ptr_recv(t: LspTest) -> None:
    """Hover on ext method with pointer receiver shows *recv_name."""
    proc = t.start_server()
    try:
        t.initialize(proc)
        src = (
            "struct Counter {\n"
            "    value: i32 = 0\n"
            "}\n"
            "ext Counter {\n"
            "    fn inc(mut *c) {\n"
            "        c.value = c.value + 1\n"
            "    }\n"
            "}\n"
            "fn main() {\n"
            "    ct := Counter {}\n"
            "    ct.inc()\n"
            "}\n"
        )
        with tempfile.NamedTemporaryFile(
            suffix=".rsg", mode="w", delete=False, encoding="utf-8"
        ) as f:
            f.write(src)
            tmp_path = f.name
        try:
            uri = file_uri(tmp_path)
            notification(proc, "textDocument/didOpen", {
                "textDocument": {"uri": uri, "languageId": "resurg", "version": 1, "text": src},
            })
            _diag = read_message(proc, timeout=5.0)
            if _diag is None:
                t.fail("hover ext ptr recv: no publishDiagnostics after didOpen")
                return

            # Hover over "inc" on line 10, character 7.
            request(proc, 21, "textDocument/hover", {
                "textDocument": {"uri": uri},
                "position": {"line": 10, "character": 7},
            })
            resp = read_message(proc, timeout=5.0)
            if resp is None or "result" not in resp:
                t.fail("hover ext ptr recv: no response")
                return
            result = resp["result"]
            if result is None:
                t.fail("hover ext ptr recv: result is null")
                return
            value = result.get("contents", {}).get("value", "")
            if "ext Counter" not in value:
                t.fail("hover ext ptr recv: missing 'ext Counter' context", value)
                return
            if "mut *c" not in value:
                t.fail("hover ext ptr recv: missing 'mut *c' receiver", value)
                return
            t.ok("hover ext ptr recv: shows ext context and pointer receiver")
        finally:
            os.unlink(tmp_path)
    finally:
        proc.kill()
        proc.wait()


def test_hover_struct(t: LspTest) -> None:
    """Hover on struct name shows field layout."""
    proc = t.start_server()
    try:
        t.initialize(proc)
        src = (
            "struct Point {\n"
            "    x: f64\n"
            "    y: f64\n"
            "}\n"
            "fn main() {\n"
            "    p := Point { x = 1.0, y = 2.0 }\n"
            "}\n"
        )
        with tempfile.NamedTemporaryFile(
            suffix=".rsg", mode="w", delete=False, encoding="utf-8"
        ) as f:
            f.write(src)
            tmp_path = f.name
        try:
            uri = file_uri(tmp_path)
            notification(proc, "textDocument/didOpen", {
                "textDocument": {"uri": uri, "languageId": "resurg", "version": 1, "text": src},
            })
            _diag = read_message(proc, timeout=5.0)
            if _diag is None:
                t.fail("hover struct: no publishDiagnostics after didOpen")
                return

            # Hover over "Point" on line 5, character 9.
            request(proc, 22, "textDocument/hover", {
                "textDocument": {"uri": uri},
                "position": {"line": 5, "character": 9},
            })
            resp = read_message(proc, timeout=5.0)
            if resp is None or "result" not in resp:
                t.fail("hover struct: no response")
                return
            result = resp["result"]
            if result is None:
                t.fail("hover struct: result is null")
                return
            value = result.get("contents", {}).get("value", "")
            if "Point" not in value:
                t.fail("hover struct: missing struct name", value)
                return
            if "x" not in value or "y" not in value:
                t.fail("hover struct: missing fields", value)
                return
            t.ok("hover struct: shows struct name and fields")
        finally:
            os.unlink(tmp_path)
    finally:
        proc.kill()
        proc.wait()


def test_hover_pact(t: LspTest) -> None:
    """Hover on pact name shows pact info."""
    proc = t.start_server()
    try:
        t.initialize(proc)
        src = (
            "/// A describable thing.\n"
            "pact Describe {\n"
            "    fn describe() -> str\n"
            "}\n"
            "fn show<T: Describe>(v: T) -> str {\n"
            "    return v.describe()\n"
            "}\n"
            "fn main() {\n"
            "    x := 1\n"
            "}\n"
        )
        with tempfile.NamedTemporaryFile(
            suffix=".rsg", mode="w", delete=False, encoding="utf-8"
        ) as f:
            f.write(src)
            tmp_path = f.name
        try:
            uri = file_uri(tmp_path)
            notification(proc, "textDocument/didOpen", {
                "textDocument": {"uri": uri, "languageId": "resurg", "version": 1, "text": src},
            })
            _diag = read_message(proc, timeout=5.0)
            if _diag is None:
                t.fail("hover pact: no publishDiagnostics after didOpen")
                return

            # Hover over "Describe" on line 4, character 14.
            request(proc, 23, "textDocument/hover", {
                "textDocument": {"uri": uri},
                "position": {"line": 4, "character": 14},
            })
            resp = read_message(proc, timeout=5.0)
            if resp is None or "result" not in resp:
                t.fail("hover pact: no response")
                return
            result = resp["result"]
            if result is None:
                t.fail("hover pact: result is null")
                return
            value = result.get("contents", {}).get("value", "")
            if "Describe" not in value:
                t.fail("hover pact: missing pact name", value)
                return
            if "describable" not in value.lower():
                t.fail("hover pact: missing doc comment", value)
                return
            t.ok("hover pact: shows pact name and doc comment")
        finally:
            os.unlink(tmp_path)
    finally:
        proc.kill()
        proc.wait()


ALL_TESTS = [
    test_initialize,
    test_shutdown_exit,
    test_didopen_clean_file,
    test_didopen_error_file,
    test_didchange,
    test_diagnostics_severity,
    test_diagnostics_message_content,
    test_hover,
    test_hover_ext_value_recv,
    test_hover_ext_ptr_recv,
    test_hover_struct,
    test_hover_pact,
    test_definition,
    test_document_symbol,
]


def main() -> int:
    parser = argparse.ArgumentParser(description="LSP integration tests")
    parser.add_argument("--lsp", default=None, help="Path to rsg-lsp binary")
    parser.add_argument("--std-path", default=None, help="Path to std library")
    args = parser.parse_args()

    # Discover LSP binary.
    lsp_path = args.lsp
    if lsp_path is None:
        build_dir = os.path.join(os.path.dirname(__file__), "..", "..", "build")
        lsp_path = os.path.join(build_dir, f"rsg-lsp{EXE_EXT}")
    if not os.path.isfile(lsp_path):
        print(f"# rsg-lsp not found at: {lsp_path}", file=sys.stderr)
        print("TAP version 13\n1..0\n# SKIP rsg-lsp binary not found")
        return 0

    t = LspTest(lsp_path, args.std_path)
    t.start(len(ALL_TESTS))
    for test_fn in ALL_TESTS:
        try:
            test_fn(t)
        except Exception as e:
            t.fail(test_fn.__name__, str(e))
    return t.summary()


if __name__ == "__main__":
    sys.exit(main())
