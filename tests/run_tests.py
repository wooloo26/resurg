#!/usr/bin/env python3
"""Unified test runner for the Resurg compiler.

Replaces run_test.sh, run_test.ps1, and run_all_tests.ps1 with a single
cross-platform program that produces TAP (Test Anything Protocol) output.

Supported directives (parsed from leading ``//`` comments):
    // TEST: compile_error      — resurg must exit != 0
    // TEST: runtime_error      — binary must exit != 0
    // EXPECT-ERROR: <text>     — stderr must contain <text>
    // EXPECT-STDOUT: <text>    — stdout must contain <text>
    // TIMEOUT: <seconds>       — per-test timeout override
    // SKIP: <reason>           — skip this test unconditionally
    // DEPENDS: <file.rsg>      — additional source files to compile

Usage:
    python3 tests/run_tests.py                       # run all tests
    python3 tests/run_tests.py tests/integration/v0.1.0/primitives.rsg
    python3 tests/run_tests.py --format=tap          # TAP output (default)
    python3 tests/run_tests.py --format=junit        # JUnit XML output
    python3 tests/run_tests.py --jobs=4              # parallel execution
    python3 tests/run_tests.py --cache               # skip unchanged tests
"""

from __future__ import annotations

import argparse
import glob
import os
import platform
import re
import signal
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from concurrent.futures import ProcessPoolExecutor, as_completed
from dataclasses import dataclass, field
from pathlib import Path

# ── Defaults ────────────────────────────────────────────────────────────

DEFAULT_TIMEOUT = int(os.environ.get("RSG_TEST_TIMEOUT", "10"))
DEFAULT_CC = "clang"
IS_WINDOWS = platform.system() == "Windows"
EXE_EXT = ".exe" if IS_WINDOWS else ""

# ── Directive parsing ───────────────────────────────────────────────────

# Registry of directive parsers.  Each entry maps a regex to a handler
# that receives (match, directives_dict).  Adding a new directive is a
# one-liner in _DIRECTIVE_TABLE — no other code needs to change.

_DIRECTIVE_TABLE: list[tuple[re.Pattern[str], str, bool]] = [
    # (compiled regex, dict key, is_list)
    (re.compile(r"^//\s*TEST:\s*(.+)"),          "test_mode",     False),
    (re.compile(r"^//\s*EXPECT-ERROR:\s*(.+)"),  "expect_error",  False),
    (re.compile(r"^//\s*EXPECT-STDOUT:\s*(.+)"), "expect_stdout", True),
    (re.compile(r"^//\s*TIMEOUT:\s*(\d+)"),      "timeout",       False),
    (re.compile(r"^//\s*SKIP:\s*(.+)"),          "skip",          False),
    (re.compile(r"^//\s*DEPENDS:\s*(.+)"),       "depends",       True),
]

# Section marker regex for multi-test files.
SECTION_RE = re.compile(r"^//\s*===\s*(.+?)\s*===\s*$")


def parse_directives(path: Path) -> dict:
    """Extract directives from the leading comment block of a .rsg file."""
    directives: dict = {
        "test_mode": "normal",
        "expect_error": "",
        "expect_stdout": [],
        "timeout": None,
        "skip": None,
        "depends": [],
    }
    with open(path, encoding="utf-8") as f:
        for raw_line in f:
            line = raw_line.rstrip("\n")
            matched = False
            for pattern, key, is_list in _DIRECTIVE_TABLE:
                m = pattern.match(line)
                if m:
                    value = m.group(1).strip()
                    if is_list:
                        directives[key].append(value)
                    else:
                        directives[key] = value
                    matched = True
                    break
            if not matched:
                if line.startswith("//") or line == "":
                    continue
                break
    return directives


# ── Section support ─────────────────────────────────────────────────────

def section_names(path: Path) -> list[str]:
    """Return section names if the file contains ``// === name ===`` markers."""
    names: list[str] = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = SECTION_RE.match(line.rstrip("\n"))
            if m:
                names.append(m.group(1).strip())
    return names


def extract_section_content(path: Path, name: str) -> str:
    """Return preamble + named section text for compilation.

    The *preamble* is every line before the first section marker (typically
    file-level directives like ``// TEST: compile_error``).  The section
    body is everything between the requested ``// === name ===`` marker and
    the next marker (or EOF).
    """
    lines = path.read_text(encoding="utf-8").splitlines(keepends=True)
    preamble: list[str] = []
    found = False
    section: list[str] = []
    seen_any_section = False
    for line in lines:
        m = SECTION_RE.match(line.rstrip("\n"))
        if m:
            if found:
                break  # reached next section
            seen_any_section = True
            if m.group(1).strip() == name:
                found = True
            continue  # always skip the marker line itself
        if not seen_any_section:
            preamble.append(line)
        elif found:
            section.append(line)
        # else: skip lines belonging to other sections
    if not found:
        raise ValueError(f"section '{name}' not found in {path}")
    return "".join(preamble + section)


# ── Result dataclass ────────────────────────────────────────────────────

@dataclass
class TestResult:
    name: str
    passed: bool
    skipped: bool = False
    skip_reason: str = ""
    cached: bool = False
    message: str = ""
    command: str = ""
    output: str = ""
    duration: float = 0.0


# ── Signal description ──────────────────────────────────────────────────

_SIGNAL_NAMES = {
    6: "SIGABRT (abort)",
    8: "SIGFPE (floating point exception)",
    9: "SIGKILL (killed)",
    11: "SIGSEGV (segmentation fault)",
}


def describe_exit(code: int) -> str:
    if code < 0:
        sig = -code
        return _SIGNAL_NAMES.get(sig, f"signal {sig}")
    if code > 128:
        sig = code - 128
        return _SIGNAL_NAMES.get(sig, f"signal {sig}")
    return f"exit code {code}"


# ── Build-artifact caching ──────────────────────────────────────────────

def is_cached(rsg_path: Path, c_path: Path, bin_path: Path) -> bool:
    """Return True when generated .c and binary are newer than the source."""
    if not c_path.exists() or not bin_path.exists():
        return False
    src_mtime = rsg_path.stat().st_mtime
    return (c_path.stat().st_mtime >= src_mtime
            and bin_path.stat().st_mtime >= src_mtime)


# ── Single-test execution ──────────────────────────────────────────────

def run_one_test(
    test_id: str,
    resurg: str,
    cc: str,
    rt_objs: str,
    build_dir: str,
    runtime_dir: str,
    use_cache: bool,
) -> TestResult:
    """Execute a single .rsg test (or section) and return the result."""
    result = TestResult(name=test_id, passed=False)
    start = time.monotonic()

    # ── Resolve section-based test IDs ──
    if "::" in test_id:
        base_file, section_name = test_id.rsplit("::", 1)
        content = extract_section_content(Path(base_file), section_name)
        safe = re.sub(r"[^a-zA-Z0-9_]", "_", section_name)
        stem = base_file.removesuffix(".rsg")
        rsg_path = Path(build_dir) / f"{stem}__{safe}.rsg"
        rsg_path.parent.mkdir(parents=True, exist_ok=True)
        rsg_path.write_text(content, encoding="utf-8")
        out_stem = f"{stem}__{safe}"
    else:
        rsg_path = Path(test_id)
        out_stem = test_id.removesuffix(".rsg") if test_id.endswith(".rsg") else test_id

    # ── Parse directives ──
    directives = parse_directives(rsg_path)
    timeout = int(directives["timeout"]) if directives["timeout"] else DEFAULT_TIMEOUT

    # ── SKIP directive ──
    if directives["skip"]:
        result.skipped = True
        result.skip_reason = directives["skip"]
        result.passed = True
        result.duration = time.monotonic() - start
        return result

    # ── Output paths ──
    test_c = Path(build_dir) / f"{out_stem}.c"
    test_bin = Path(build_dir) / f"{out_stem}{EXE_EXT}"
    test_c.parent.mkdir(parents=True, exist_ok=True)

    rt_list = rt_objs.split() if rt_objs else []
    mode = directives["test_mode"]

    try:
        if mode == "normal":
            _run_normal(
                rsg_path, test_c, test_bin, directives,
                resurg, cc, rt_list, runtime_dir, timeout, use_cache, result,
            )
        elif mode == "compile_error":
            _run_compile_error(
                rsg_path, test_c, directives, resurg, timeout, result,
            )
        elif mode == "runtime_error":
            _run_runtime_error(
                rsg_path, test_c, test_bin, directives,
                resurg, cc, rt_list, runtime_dir, timeout, result,
            )
        else:
            result.message = f"unknown test mode: {mode}"
    except Exception as exc:
        result.message = str(exc)

    result.duration = time.monotonic() - start
    return result


# ── Mode: normal ────────────────────────────────────────────────────────

def _run_normal(
    rsg_path: Path,
    test_c: Path,
    test_bin: Path,
    directives: dict,
    resurg: str,
    cc: str,
    rt_list: list[str],
    runtime_dir: str,
    timeout: int,
    use_cache: bool,
    result: TestResult,
) -> None:
    # ── cache check ──
    if use_cache and is_cached(rsg_path, test_c, test_bin):
        # Still need to run the binary to verify behaviour.
        pass
    else:
        _codegen(rsg_path, test_c, resurg, timeout, result)
        if not result.passed and result.message:
            return
        _compile_c(test_c, test_bin, cc, rt_list, runtime_dir, result)
        if not result.passed and result.message:
            return

    # Run binary
    cmd = [str(test_bin)]
    try:
        proc = subprocess.run(
            cmd, capture_output=True, text=True, timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        result.message = f"test binary timed out ({timeout}s limit)"
        result.command = str(test_bin)
        return

    if proc.returncode != 0:
        result.message = f"test binary crashed ({describe_exit(proc.returncode)})"
        result.command = str(test_bin)
        result.output = proc.stderr
        return

    # ── EXPECT-STDOUT assertions ──
    for expected in directives["expect_stdout"]:
        if expected not in proc.stdout:
            result.message = f"expected stdout: {expected}"
            result.output = f"got: {proc.stdout}"
            return

    result.passed = True


# ── Mode: compile_error ─────────────────────────────────────────────────

def _run_compile_error(
    rsg_path: Path,
    test_c: Path,
    directives: dict,
    resurg: str,
    timeout: int,
    result: TestResult,
) -> None:
    cmd = [resurg, str(rsg_path), "-o", str(test_c)]
    try:
        proc = subprocess.run(
            cmd, capture_output=True, text=True, timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        result.message = f"resurg timed out ({timeout}s limit)"
        result.command = " ".join(cmd)
        return

    if proc.returncode == 0:
        result.message = "expected compile error but resurg succeeded"
        return

    expect = directives["expect_error"]
    if expect and expect not in proc.stderr:
        result.message = f"expected error: {expect}"
        result.command = " ".join(cmd)
        result.output = f"got: {proc.stderr}"
        return

    result.passed = True


# ── Mode: runtime_error ─────────────────────────────────────────────────

def _run_runtime_error(
    rsg_path: Path,
    test_c: Path,
    test_bin: Path,
    directives: dict,
    resurg: str,
    cc: str,
    rt_list: list[str],
    runtime_dir: str,
    timeout: int,
    result: TestResult,
) -> None:
    _codegen(rsg_path, test_c, resurg, timeout, result)
    if not result.passed and result.message:
        return
    _compile_c(test_c, test_bin, cc, rt_list, runtime_dir, result)
    if not result.passed and result.message:
        return

    cmd = [str(test_bin)]
    try:
        proc = subprocess.run(
            cmd, capture_output=True, text=True, timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        result.message = f"test binary timed out ({timeout}s limit)"
        result.command = str(test_bin)
        return

    if proc.returncode == 0:
        result.message = "expected runtime error but program succeeded"
        return

    result.passed = True


# ── Shared helpers ──────────────────────────────────────────────────────

def _codegen(
    rsg_path: Path, test_c: Path, resurg: str, timeout: int, result: TestResult,
) -> None:
    cmd = [resurg, str(rsg_path), "-o", str(test_c)]
    result.command = " ".join(cmd)
    try:
        proc = subprocess.run(
            cmd, capture_output=True, text=True, timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        result.message = f"resurg timed out ({timeout}s limit)"
        return
    if proc.returncode != 0:
        result.message = f"resurg codegen failed ({describe_exit(proc.returncode)})"
        result.output = proc.stderr
        return
    # Reset — codegen succeeded; caller will continue.
    result.command = ""


def _compile_c(
    test_c: Path,
    test_bin: Path,
    cc: str,
    rt_list: list[str],
    runtime_dir: str,
    result: TestResult,
) -> None:
    cmd = [
        cc, "-std=c17", "-Wno-tautological-compare",
        f"-I{runtime_dir}", "-o", str(test_bin), str(test_c),
    ] + rt_list
    result.command = " ".join(cmd)
    try:
        proc = subprocess.run(
            cmd, capture_output=True, text=True, timeout=60,
        )
    except subprocess.TimeoutExpired:
        result.message = "C compilation timed out"
        return
    if proc.returncode != 0:
        result.message = "C compilation failed"
        result.output = proc.stderr
        return
    result.command = ""


# ── Output formatters ──────────────────────────────────────────────────

COLORS = {
    "pass":  "\033[1;32m",
    "fail":  "\033[1;31m",
    "skip":  "\033[1;33m",
    "reset": "\033[0m",
}

# Disable colours when not connected to a terminal or on dumb terminals.
if not sys.stdout.isatty() or os.environ.get("NO_COLOR") or os.environ.get("TERM") == "dumb":
    COLORS = {k: "" for k in COLORS}


def format_tap(results: list[TestResult]) -> str:
    """Format results as TAP (Test Anything Protocol) version 13."""
    lines = [f"TAP version 13", f"1..{len(results)}"]
    for i, r in enumerate(results, 1):
        if r.skipped:
            lines.append(f"ok {i} - {r.name} # SKIP {r.skip_reason}")
        elif r.passed:
            suffix = " # cached" if r.cached else ""
            lines.append(f"ok {i} - {r.name}{suffix}")
        else:
            lines.append(f"not ok {i} - {r.name}")
            lines.append("  ---")
            lines.append(f"  message: {r.message}")
            if r.command:
                lines.append(f"  command: {r.command}")
            if r.output:
                for ol in r.output.strip().splitlines():
                    lines.append(f"  output: {ol}")
            lines.append("  ...")
    return "\n".join(lines)


def format_junit(results: list[TestResult]) -> str:
    """Format results as JUnit XML."""
    suite = ET.Element("testsuite", {
        "name": "resurg",
        "tests": str(len(results)),
        "failures": str(sum(1 for r in results if not r.passed and not r.skipped)),
        "skipped": str(sum(1 for r in results if r.skipped)),
        "time": f"{sum(r.duration for r in results):.3f}",
    })
    for r in results:
        case = ET.SubElement(suite, "testcase", {
            "name": r.name,
            "time": f"{r.duration:.3f}",
        })
        if r.skipped:
            ET.SubElement(case, "skipped", {"message": r.skip_reason})
        elif not r.passed:
            fail_el = ET.SubElement(case, "failure", {"message": r.message})
            parts = []
            if r.command:
                parts.append(f"cmd: {r.command}")
            if r.output:
                parts.append(f"output: {r.output.strip()}")
            fail_el.text = "\n".join(parts)
    return ET.tostring(suite, encoding="unicode", xml_declaration=True)


def print_human(results: list[TestResult]) -> None:
    """Print coloured human-readable results to stderr, TAP to stdout."""
    for r in results:
        if r.skipped:
            print(
                f"{COLORS['skip']}  SKIP  {r.name} — {r.skip_reason}{COLORS['reset']}",
                file=sys.stderr,
            )
        elif r.passed:
            tag = " (cached)" if r.cached else ""
            print(
                f"{COLORS['pass']}  PASS  {r.name}{tag}{COLORS['reset']}",
                file=sys.stderr,
            )
        else:
            print(
                f"{COLORS['fail']}  FAIL  {r.name} — {r.message}{COLORS['reset']}",
                file=sys.stderr,
            )
            if r.command:
                print(
                    f"{COLORS['fail']}         cmd: {r.command}{COLORS['reset']}",
                    file=sys.stderr,
                )
            if r.output:
                for line in r.output.strip().splitlines():
                    print(
                        f"{COLORS['fail']}         output: {line}{COLORS['reset']}",
                        file=sys.stderr,
                    )


# ── Test discovery ──────────────────────────────────────────────────────

def discover_tests(root: str) -> list[str]:
    """Find all .rsg test files under the integration test directory.

    Files containing ``// === name ===`` section markers are expanded into
    one test entry per section (``path.rsg::section_name``).
    """
    pattern = os.path.join(root, "tests", "integration", "**", "*.rsg")
    raw_files = sorted(glob.glob(pattern, recursive=True))
    result: list[str] = []
    for f in raw_files:
        names = section_names(Path(f))
        if names:
            for n in names:
                result.append(f"{f}::{n}")
        else:
            result.append(f)
    return result


def expand_tests(files: list[str]) -> list[str]:
    """Expand explicitly-provided file paths that contain sections."""
    result: list[str] = []
    for f in files:
        if "::" in f:
            result.append(f)
        else:
            p = Path(f)
            if p.exists():
                names = section_names(p)
                if names:
                    for n in names:
                        result.append(f"{f}::{n}")
                else:
                    result.append(f)
            else:
                result.append(f)
    return result


# ── CLI ─────────────────────────────────────────────────────────────────

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Resurg test runner — cross-platform, structured output",
    )
    p.add_argument(
        "files", nargs="*", metavar="FILE",
        help="Test .rsg files to run (default: all under tests/integration/)",
    )
    p.add_argument(
        "--resurg", default=None,
        help="Path to the resurg compiler binary (default: build/resurg)",
    )
    p.add_argument(
        "--cc", default=os.environ.get("CC", DEFAULT_CC),
        help="C compiler (default: clang or $CC)",
    )
    p.add_argument(
        "--rt-objs", default=None,
        help="Runtime library object/archive path",
    )
    p.add_argument(
        "--build", default="build",
        help="Build directory (default: build)",
    )
    p.add_argument(
        "--runtime", default="runtime",
        help="Runtime include directory (default: runtime)",
    )
    p.add_argument(
        "--format", choices=["tap", "junit"], default="tap",
        help="Structured output format (default: tap)",
    )
    p.add_argument(
        "--jobs", "-j", type=int, default=None,
        help="Number of parallel workers (default: CPU count)",
    )
    p.add_argument(
        "--cache", action="store_true",
        help="Skip recompilation when .rsg is older than build artifacts",
    )
    return p


def resolve_defaults(args: argparse.Namespace) -> None:
    """Fill in platform-specific defaults that depend on other args."""
    if args.resurg is None:
        args.resurg = os.path.join(args.build, f"resurg{EXE_EXT}")
    if args.rt_objs is None:
        if IS_WINDOWS:
            args.rt_objs = os.path.join(args.build, "resurg_runtime.lib")
        else:
            args.rt_objs = os.path.join(args.build, "libresurg_runtime.a")


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    resolve_defaults(args)

    # Discover or use explicit file list.
    files = expand_tests(args.files) if args.files else discover_tests(".")
    if not files:
        print("No test files found.", file=sys.stderr)
        return 1

    jobs = args.jobs if args.jobs else min(os.cpu_count() or 1, len(files))

    # ── Execute tests (parallel) ──
    results: list[TestResult] = []
    if jobs == 1:
        for f in files:
            results.append(run_one_test(
                f, args.resurg, args.cc, args.rt_objs,
                args.build, args.runtime, args.cache,
            ))
    else:
        # Use a process pool so individual test crashes don't kill the runner.
        future_map = {}
        with ProcessPoolExecutor(max_workers=jobs) as pool:
            for f in files:
                fut = pool.submit(
                    run_one_test,
                    f, args.resurg, args.cc, args.rt_objs,
                    args.build, args.runtime, args.cache,
                )
                future_map[fut] = f

            # Collect in submission order for stable TAP numbering.
            ordered_futures = [None] * len(files)
            for fut, f in future_map.items():
                idx = files.index(f)
                ordered_futures[idx] = fut

            for fut in ordered_futures:
                results.append(fut.result())

    # ── Print results ──
    print_human(results)

    # Only emit structured output (TAP/JUnit) when stdout is piped.
    if not sys.stdout.isatty():
        if args.format == "tap":
            print(format_tap(results))
        else:
            print(format_junit(results))

    # ── Summary ──
    passed = sum(1 for r in results if r.passed and not r.skipped)
    failed = sum(1 for r in results if not r.passed)
    skipped = sum(1 for r in results if r.skipped)
    cached = sum(1 for r in results if r.cached)
    total = len(results)
    elapsed = sum(r.duration for r in results)

    bar = "━" * 40
    parts = []
    if failed:
        parts.append(f"{COLORS['fail']}  {failed} failed{COLORS['reset']}")
    parts.append(f"{COLORS['pass']}  {passed} passed{COLORS['reset']}")
    if skipped:
        parts.append(f"{COLORS['skip']}  {skipped} skipped{COLORS['reset']}")
    if cached:
        parts.append(f"  {cached} cached")
    detail = ",".join(parts)
    color = COLORS['fail'] if failed else COLORS['pass']
    print(f"\n{color}{bar}{COLORS['reset']}", file=sys.stderr)
    print(f"{detail}  ({total} total, {elapsed:.1f}s)", file=sys.stderr)
    print(f"{color}{bar}{COLORS['reset']}", file=sys.stderr)

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
