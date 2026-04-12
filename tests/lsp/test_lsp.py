#!/usr/bin/env python3
"""Snapshot-based LSP integration tests for rsg-lsp.

Reads declarative .test files from tests/lsp/cases/ and validates
LSP responses against expected snapshots.

Usage:
    python3 tests/lsp/test_lsp.py [--lsp=<path>] [--std-path=<dir>] [--update] [FILE...]

.test file format
─────────────────
Sections are delimited by `-- DIRECTIVE [ARGS...] --` headers.
Lines before the first section header are ignored (comments).

Directives:
    input.rsg                Source code to open via textDocument/didOpen
    change                   New source sent via textDocument/didChange
    initialize               Assert server initializes with capabilities
    shutdown                 Assert clean shutdown/exit lifecycle
    diagnostics              Exact snapshot of diagnostics (empty = none)
    diagnostics nonempty     Assert >= 1 diagnostic
    diagnostics severity N   Assert first diagnostic has severity N (1-4)
    diagnostics contains     Each body line must appear in some message
    hover L:C                Exact hover text snapshot
    hover L:C contains       Each body line must be substring of hover
    definition L:C           Expected target as L:C (0-based)
    symbol                   Expected "NAME KIND_NUM" lines

Pass --update to overwrite exact-match sections with actual output.
Exit code 0 on success, 1 on failure.  Output is TAP v13.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
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
    content_length = None
    for line in header_bytes.decode("ascii", errors="replace").split("\r\n"):
        if line.lower().startswith("content-length:"):
            content_length = int(line.split(":", 1)[1].strip())
            break
    if content_length is None:
        return None

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
    proc.stdin.write(encode_message(obj))
    proc.stdin.flush()


def request(proc: subprocess.Popen, id: int, method: str, params: dict | None = None) -> None:
    msg: dict = {"jsonrpc": "2.0", "id": id, "method": method}
    if params is not None:
        msg["params"] = params
    send(proc, msg)


def notification(proc: subprocess.Popen, method: str, params: dict | None = None) -> None:
    msg: dict = {"jsonrpc": "2.0", "method": method}
    if params is not None:
        msg["params"] = params
    send(proc, msg)


def file_uri(path: str) -> str:
    p = Path(path).resolve().as_posix()
    if not p.startswith("/"):
        p = "/" + p
    return "file://" + p


# ── Test file parser ────────────────────────────────────────────────────

SECTION_RE = re.compile(r"^-- (.+?) --\s*$")


def parse_test_file(path: str) -> list[tuple[str, str]]:
    """Parse a .test file into [(header, body), ...]."""
    sections: list[tuple[str, str]] = []
    current_header: str | None = None
    current_lines: list[str] = []

    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.rstrip("\n")
            m = SECTION_RE.match(line)
            if m:
                if current_header is not None:
                    sections.append((current_header, _join_body(current_lines)))
                current_header = m.group(1)
                current_lines = []
            elif current_header is not None:
                current_lines.append(line)

    if current_header is not None:
        sections.append((current_header, _join_body(current_lines)))
    return sections


def _join_body(lines: list[str]) -> str:
    while lines and lines[-1] == "":
        lines.pop()
    return "\n".join(lines)


def write_test_file(path: str, sections: list[tuple[str, str]]) -> None:
    with open(path, "w", encoding="utf-8") as f:
        for header, body in sections:
            f.write(f"-- {header} --\n")
            if body:
                f.write(body + "\n")


def parse_position(s: str) -> tuple[int, int]:
    parts = s.split(":")
    return int(parts[0]), int(parts[1])


# ── Formatters ──────────────────────────────────────────────────────────

SEVERITY_NAMES = {1: "error", 2: "warning", 3: "info", 4: "hint"}


def format_diagnostics(diags: list[dict]) -> str:
    lines = []
    for d in diags:
        r = d["range"]
        s, e = r["start"], r["end"]
        sev = SEVERITY_NAMES.get(d.get("severity", 1), "unknown")
        msg = d.get("message", "")
        lines.append(f"{s['line']}:{s['character']}-{e['line']}:{e['character']} {sev} {msg}")
    return "\n".join(lines)


def format_hover(result: dict | None) -> str:
    if result is None:
        return ""
    return result.get("contents", {}).get("value", "")


def format_definition(result: dict | list | None) -> str:
    if result is None:
        return ""
    if isinstance(result, list):
        result = result[0] if result else None
    if result is None:
        return ""
    s = result["range"]["start"]
    return f"{s['line']}:{s['character']}"


def format_symbols(symbols: list[dict]) -> str:
    return "\n".join(f"{s['name']} {s['kind']}" for s in symbols)


# ── Test runner ─────────────────────────────────────────────────────────


class TestRunner:
    def __init__(self, lsp_path: str, std_path: str | None, update: bool = False):
        self.lsp_path = lsp_path
        self.std_path = std_path
        self.update = update
        self.passed = 0
        self.failed = 0
        self.test_num = 0

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
        cmd = [self.lsp_path]
        if self.std_path:
            cmd.append(f"--std-path={self.std_path}")
        return subprocess.Popen(
            cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )

    def server_initialize(self, proc: subprocess.Popen) -> dict | None:
        request(proc, 1, "initialize", {
            "processId": os.getpid(),
            "rootUri": file_uri(os.getcwd()),
            "capabilities": {},
        })
        resp = read_message(proc)
        if resp is not None:
            notification(proc, "initialized", {})
        return resp

    def await_diagnostics(self, proc: subprocess.Popen, timeout: float = 5.0) -> list[dict] | None:
        """Read messages until publishDiagnostics or timeout."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            remaining = max(0.5, deadline - time.time())
            msg = read_message(proc, timeout=remaining)
            if msg is None:
                return None
            if msg.get("method") == "textDocument/publishDiagnostics":
                return msg["params"]["diagnostics"]
        return None

    def read_response(self, proc: subprocess.Popen, expected_id: int, timeout: float = 5.0) -> dict | None:
        """Read messages until a response with the expected id."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            remaining = max(0.5, deadline - time.time())
            msg = read_message(proc, timeout=remaining)
            if msg is None:
                return None
            if "id" in msg and msg["id"] == expected_id:
                return msg
        return None

    # ── Snapshot comparison helpers ──

    def check_exact(self, name: str, header: str, expected: str, actual: str) -> tuple[bool, str]:
        """Compare expected vs actual. Returns (passed, body_to_write)."""
        if expected == actual:
            self.ok(f"{name}: {header}")
            return True, expected
        if self.update:
            self.ok(f"{name}: {header} (updated)")
            return True, actual
        self.fail(f"{name}: {header}", f"expected:\n{expected}\nactual:\n{actual}")
        return False, expected

    def check_contains(self, name: str, header: str, body: str, actual: str) -> bool:
        needles = [l for l in body.split("\n") if l.strip()]
        missing = [n for n in needles if n not in actual]
        if missing:
            self.fail(f"{name}: {header}: missing: {missing}", f"actual:\n{actual}")
            return False
        self.ok(f"{name}: {header}")
        return True

    # ── Main executor ──

    def run_test_file(self, path: str) -> list[tuple[str, str]] | None:
        sections = parse_test_file(path)
        if not sections:
            return None

        name = Path(path).stem
        proc = self.start_server()
        rid = 10
        uri = None
        tmp_paths: list[str] = []
        initialized = False
        cached_diags: list[dict] | None = None
        results: list[tuple[str, str]] = []
        any_updated = False

        try:
            for header, body in sections:
                parts = header.split()
                directive = parts[0]

                # Auto-initialize for non-initialize directives.
                if directive != "initialize" and not initialized:
                    resp = self.server_initialize(proc)
                    initialized = True
                    if resp is None or "result" not in resp:
                        self.fail(f"{name}: auto-initialize failed")
                        results.append((header, body))
                        continue

                # ── input.rsg ──
                if header == "input.rsg":
                    src = body + "\n" if body else ""
                    with tempfile.NamedTemporaryFile(
                        suffix=".rsg", mode="w", delete=False, encoding="utf-8",
                    ) as f:
                        f.write(src)
                        tmp_paths.append(f.name)
                    uri = file_uri(tmp_paths[-1])
                    notification(proc, "textDocument/didOpen", {
                        "textDocument": {
                            "uri": uri, "languageId": "resurg",
                            "version": 1, "text": src,
                        },
                    })
                    cached_diags = self.await_diagnostics(proc)
                    results.append((header, body))

                # ── change ──
                elif directive == "change":
                    new_src = body + "\n" if body else ""
                    notification(proc, "textDocument/didChange", {
                        "textDocument": {"uri": uri, "version": 2},
                        "contentChanges": [{"text": new_src}],
                    })
                    cached_diags = self.await_diagnostics(proc)
                    results.append((header, body))

                # ── initialize ──
                elif directive == "initialize":
                    resp = self.server_initialize(proc)
                    initialized = True
                    if resp is None:
                        self.fail(f"{name}: initialize: no response")
                    elif "result" not in resp:
                        self.fail(f"{name}: initialize: no result")
                    elif "textDocumentSync" not in resp["result"].get("capabilities", {}):
                        self.fail(
                            f"{name}: initialize: missing textDocumentSync",
                            json.dumps(resp["result"].get("capabilities", {}), indent=2),
                        )
                    else:
                        self.ok(f"{name}: initialize")
                    results.append((header, body))

                # ── shutdown ──
                elif directive == "shutdown":
                    request(proc, 2, "shutdown")
                    resp = read_message(proc)
                    if resp is None or resp.get("id") != 2:
                        self.fail(f"{name}: shutdown: no valid response")
                        results.append((header, body))
                        continue
                    notification(proc, "exit")
                    try:
                        proc.wait(timeout=5)
                        if proc.returncode != 0:
                            self.fail(f"{name}: exit: code {proc.returncode}")
                        else:
                            self.ok(f"{name}: shutdown/exit")
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait()
                        self.fail(f"{name}: exit: timeout")
                    results.append((header, body))

                # ── diagnostics ──
                elif directive == "diagnostics":
                    if cached_diags is None:
                        self.fail(f"{name}: {header}: no publishDiagnostics received")
                        results.append((header, body))
                        continue

                    modifier = parts[1] if len(parts) > 1 else None

                    if modifier == "nonempty":
                        if len(cached_diags) == 0:
                            self.fail(f"{name}: {header}: got 0 diagnostics")
                        else:
                            self.ok(f"{name}: {header}")
                        results.append((header, body))

                    elif modifier == "contains":
                        all_msgs = "\n".join(d.get("message", "") for d in cached_diags)
                        self.check_contains(name, header, body, all_msgs)
                        results.append((header, body))

                    elif modifier == "severity":
                        expected_sev = int(parts[2])
                        if len(cached_diags) == 0:
                            self.fail(f"{name}: {header}: no diagnostics")
                        elif cached_diags[0].get("severity") != expected_sev:
                            self.fail(
                                f"{name}: {header}: "
                                f"expected {expected_sev}, got {cached_diags[0].get('severity')}",
                            )
                        else:
                            self.ok(f"{name}: {header}")
                        results.append((header, body))

                    else:
                        # Exact snapshot match.
                        actual = format_diagnostics(cached_diags)
                        _, out_body = self.check_exact(name, header, body, actual)
                        if out_body != body:
                            any_updated = True
                        results.append((header, out_body))

                # ── hover ──
                elif directive == "hover":
                    pos_str = parts[1]
                    mode = parts[2] if len(parts) > 2 else "exact"
                    line, col = parse_position(pos_str)

                    rid += 1
                    request(proc, rid, "textDocument/hover", {
                        "textDocument": {"uri": uri},
                        "position": {"line": line, "character": col},
                    })
                    resp = self.read_response(proc, rid)

                    if resp is None or "result" not in resp:
                        self.fail(f"{name}: {header}: no response")
                        results.append((header, body))
                        continue

                    result = resp["result"]
                    if result is None:
                        self.fail(f"{name}: {header}: null result")
                        results.append((header, body))
                        continue

                    actual = format_hover(result)

                    if mode == "contains":
                        self.check_contains(name, header, body, actual)
                        results.append((header, body))
                    else:
                        _, out_body = self.check_exact(name, header, body, actual)
                        if out_body != body:
                            any_updated = True
                        results.append((header, out_body))

                # ── definition ──
                elif directive == "definition":
                    pos_str = parts[1]
                    line, col = parse_position(pos_str)

                    rid += 1
                    request(proc, rid, "textDocument/definition", {
                        "textDocument": {"uri": uri},
                        "position": {"line": line, "character": col},
                    })
                    resp = self.read_response(proc, rid)

                    if resp is None or "result" not in resp:
                        self.fail(f"{name}: {header}: no response")
                        results.append((header, body))
                        continue

                    result = resp["result"]
                    if result is None:
                        self.fail(f"{name}: {header}: null result")
                        results.append((header, body))
                        continue

                    actual = format_definition(result)
                    _, out_body = self.check_exact(name, header, body, actual)
                    if out_body != body:
                        any_updated = True
                    results.append((header, out_body))

                # ── symbol ──
                elif directive == "symbol":
                    rid += 1
                    request(proc, rid, "textDocument/documentSymbol", {
                        "textDocument": {"uri": uri},
                    })
                    resp = self.read_response(proc, rid)

                    if resp is None or "result" not in resp:
                        self.fail(f"{name}: {header}: no response")
                        results.append((header, body))
                        continue

                    symbols = resp["result"]
                    if not isinstance(symbols, list):
                        self.fail(f"{name}: {header}: result not a list")
                        results.append((header, body))
                        continue

                    actual = format_symbols(symbols)
                    _, out_body = self.check_exact(name, header, body, actual)
                    if out_body != body:
                        any_updated = True
                    results.append((header, out_body))

                else:
                    self.fail(f"{name}: unknown directive '{directive}'")
                    results.append((header, body))

        finally:
            for tp in tmp_paths:
                if os.path.exists(tp):
                    os.unlink(tp)
            try:
                proc.kill()
                proc.wait()
            except Exception:
                pass

        return results if self.update and any_updated else None


# ── Helpers ─────────────────────────────────────────────────────────────


def count_assertions(path: str) -> int:
    """Count assertion sections (non-input, non-change) in a test file."""
    sections = parse_test_file(path)
    return sum(1 for h, _ in sections if h.split()[0] not in ("input.rsg", "change"))


# ── Main ────────────────────────────────────────────────────────────────


def main() -> int:
    parser = argparse.ArgumentParser(description="Snapshot LSP tests")
    parser.add_argument("--lsp", default=None, help="Path to rsg-lsp binary")
    parser.add_argument("--std-path", default=None, help="Path to std library")
    parser.add_argument("--update", action="store_true", help="Update snapshots")
    parser.add_argument("files", nargs="*", help="Specific .test files to run")
    args = parser.parse_args()

    lsp_path = args.lsp
    if lsp_path is None:
        build_dir = os.path.join(os.path.dirname(__file__), "..", "..", "build")
        lsp_path = os.path.join(build_dir, f"rsg-lsp{EXE_EXT}")

    if not os.path.isfile(lsp_path):
        print(f"# rsg-lsp not found at: {lsp_path}", file=sys.stderr)
        print("TAP version 13\n1..0\n# SKIP rsg-lsp binary not found")
        return 0

    if args.files:
        test_files = args.files
    else:
        cases_dir = os.path.join(os.path.dirname(__file__), "cases")
        if not os.path.isdir(cases_dir):
            print(f"# cases directory not found: {cases_dir}", file=sys.stderr)
            print("TAP version 13\n1..0\n# SKIP no test cases")
            return 0
        test_files = sorted(
            os.path.join(cases_dir, f)
            for f in os.listdir(cases_dir)
            if f.endswith(".test")
        )

    total = sum(count_assertions(p) for p in test_files)
    print(f"TAP version 13\n1..{total}")

    runner = TestRunner(lsp_path, args.std_path, update=args.update)
    for path in test_files:
        updated = runner.run_test_file(path)
        if updated is not None:
            write_test_file(path, updated)

    total_run = runner.passed + runner.failed
    print(f"# {runner.passed}/{total_run} tests passed", file=sys.stderr)
    return 0 if runner.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
