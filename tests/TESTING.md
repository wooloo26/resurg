# Testing

## Running Tests

```sh
make test          # run all tests
make run FILE=tests/v0.1.0/primitives.rsg   # run a single file
```

On success the test prints `PASS`.

## Test File Conventions

- Header comment: `// v0.X.0 — Description`
- Module declaration: `module test_<feature>`
- No `main()` needed — the compiler auto-generates one that calls all `test_*` / `main` functions

## Expected-Failure Tests

To test **expected failures**, use exit-code + stderr matching — no compiler changes needed.

### Directives

Embed expected behavior in `.rsg` file comments:

```rouge name=tests/compile_error/e_undefined_var.rsg
// TEST: compile_error
// EXPECT-ERROR: undefined variable 'x'
module test_err

fn main() {
    y := x + 1
}
```

- `// TEST: compile_error` — `resurg` must exit ≠ 0, stderr must contain `EXPECT-ERROR`.
- `// TEST: runtime_error` — `resurg` succeeds, but the compiled binary must exit ≠ 0.

## Project Structure

```
tests/
├── TESTING.md
├── vx.x.x/
│   └── *.rsg
├── errors/
│   └── *.rsg
└── runtime_errors/
    └── *.rsg
```