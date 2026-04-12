# Testing

## Running Tests

```sh
make test                     # run all tests (parallel, TAP output)
make test-one FILE=tests/integration/lang/primitives.rsg  # single test
make clean-tests              # remove test build artifacts only
```

The runner (`tests/run_tests.py`) works on Linux, macOS, and Windows with
Python 3.8+.  No extra dependencies required.

### Output Formats

```sh
# TAP (default) — human-readable lines on stderr, TAP v13 on stdout
python3 tests/run_tests.py

# JUnit XML — pipe to a file for CI upload
python3 tests/run_tests.py --format=junit > test-results.xml
```

### Build-Artifact Caching

Pass `--cache` to skip recompilation when the `.rsg` source is older than its
generated `.c` and binary:

```sh
python3 tests/run_tests.py --cache
```

## Test File Conventions

- Header comment: `// Name — Description`
- Module declaration: `module test_<feature>`
- No `main()` needed — the compiler auto-generates one that calls all `test_*` / `main` functions

## Directives

Embed expected behavior in `.rsg` file comments:

| Directive                  | Description                                                    |
| -------------------------- | -------------------------------------------------------------- |
| `// TEST: compile_error`   | `resurg` must exit ≠ 0                                         |
| `// TEST: runtime_error`   | Compiled binary must exit ≠ 0                                  |
| `// EXPECT-ERROR: <text>`  | stderr must contain `<text>`                                   |
| `// EXPECT-STDOUT: <text>` | stdout must contain `<text>` (repeatable)                      |
| `// TIMEOUT: <seconds>`    | Per-test timeout override (default: 10s / `$RSG_TEST_TIMEOUT`) |
| `// SKIP: <reason>`        | Skip this test unconditionally                                 |
| `// DEPENDS: <file.rsg>`   | Additional source file for multi-file tests (repeatable)       |

### Examples

```rouge name=tests/integration/errors/e_undefined_var.rsg
// TEST: compile_error
// EXPECT-ERROR: undefined variable 'x'
module test_err

fn main() {
    y := x + 1
}
```

```rouge name=tests/integration/temp/hello.rsg
// EXPECT-STDOUT: hello world
module test_hello

fn main() {
    println("hello world")
}
```

```rouge name=tests/integration/slow_test.rsg
// TIMEOUT: 30
// SKIP: requires network
module test_net
```

## Project Structure

```plain
tests/
├── TESTING.md
├── run_tests.py          # unified test runner (Python)
├── integration/
│   ├── syntax_type/
│   │   └── *.rsg
│   ├── errors/
│   │   └── *.rsg
│   └── runtime_errors/
│       └── *.rsg
```
