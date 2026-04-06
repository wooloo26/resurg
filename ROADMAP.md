# Resurg Language Roadmap

## v0.1.0 — Bootstrap

Core compiler infrastructure and minimal language subset.

- [x] Lexer (tokenization of keywords, operators, literals)
- [x] Parser (AST for expressions, statements, declarations)
- [x] C17 code generation (basic emit pipeline)
- [x] Primitive types: `bool`, `i32`, `u32`, `f64`, `str`, `unit`
- [x] Variable declarations (`:=`, `var`)
- [x] Type inference for primitives
- [x] Functions (expression and block bodies)
- [x] `if` / `else` expressions
- [x] `loop`, `break`, `continue`
- [x] `for` with range (`0..N`)
- [x] Basic arithmetic and comparison operators
- [x] Comments (`//`)
- [x] String litexrals and interpolation
- [x] Module declaration (`module`)
- [x] `pub` visibility
- [x] `assert` (built-in function, runtime assertion)

## v0.2.0 — Type System Foundations

Compound types and core type checking.

- [x] Full integer types (`i8`–`i128`, `u8`–`u128`, `isize`, `usize`)
- [x] `f32` support
- [x] `char` type
- [x] Arrays (`[N]T`) — fixed-size value types
- [x] Tuples (`(A, B, …)`)
- [x] Type aliases (`type ID = u64`)
- [x] Explicit type annotations on declarations
- [x] No implicit conversions (enforce)
- [x] Operator precedence (full table)

## v0.3.0 — Structs & Methods

Data modeling and value semantics.

- [x] Struct definition with default field values
- [x] Struct instantiation and field access
- [x] Methods with `*` receiver (self)
- [x] Struct embedding and field/method promotion
- [x] Method override on embedded types
- [x] Destructuring (`{name, age} := user`)
- [x] Tuple destructuring (`(a, b) := pair`)
- [x] Named arguments at call site

## v0.4.0 — Memory Model

Pointer semantics and garbage collection.

- [x] `&` heap allocation (returns `*T`)
- [x] Pointer type `*T` (no arithmetic)
- [x] Auto-deref on field access (`p.field` on `*T`)
- [x] Value-type copy semantics on assignment
- [x] Pointer copy semantics (aliasing)
- [x] Addressability rules (variables addressable, rvalues not)
- [x] `mut` pointer parameters (declaration + call site)
- [x] `immut` bindings and fields
- [x] `immut` return values
- [x] Pointer-to-pointer `**T` (reassign inner pointer from callee)

## v0.4.1 — Tracing Garbage Collector

- [x] Tracing GC integration

## v0.5.0 — Enums & Pattern Matching

Algebraic data types and exhaustive matching.

- [x] Unit enum variants (`Quit`)
- [x] Tuple enum variants (`Write(str)`)
- [x] Struct enum variants (`Move { x: i32, y: i32 }`)
- [x] Enum with explicit discriminants (`Active = 1`)
- [x] `match` expression (exhaustiveness checking)
- [x] Literal, range, wildcard, binding patterns
- [x] Variant patterns (unit, tuple, fields)
- [x] Match guards (`pattern if cond => ...`)
- [x] Compile-time rejection of non-exhaustive matches
- [x] `::` namespace operator for enum variant access (`Enum::Variant`)

## v0.5.1 — Control flow & Other proposals

- [x] `never` type
- [x] scope & shadowing & expression
- [x] declare first
- [x] `unit` is empty tuple
- [x] `while` loop
- [x] `loop` expression
- [x] `defer` (LIFO cleanup on scope exit)
- [x] `return` early return

## v0.5.2 — Escape Analysis

- [x] Escape Analysis (if not proven non-escaping, it escapes)
  
## v0.6.0 — Slices & Collections

Dynamic-length data.

- [x] Slice type `[]T` (fat pointer: data + length)
- [x] Array-to-slice conversion (`arr[..]`)
- [x] Sub-slicing (`s[1..4]`, `s[2..]`, `s[..3]`)
- [x] Shared backing storage semantics
- [x] Slice of slices (`[][]T`)
- [x] Slice pointer `*[]T` (reassign slice header in-place)
- [x] `for` over slices with value and index (`|v, i|`)

## v0.7.0 — Pacts & Conformance

- [x] Pact (interface) definition with required fields and methods
- [x] Default method implementations in pacts
- [x] Explicit conformance (`struct Foo: Pact1 + Pact2`)
- [x] Constraint aliases (`pact A { Ord; Display } or pact A = Ord + Display`)
- [x] Value receivers
  
## v0.8.0 — Generics Core

- [ ] Generic function syntax (`fn max<T: Ord>(a: T, b: T) -> T`)
- [ ] Pact-bounded type parameters (`T: Ord + Display`)
- [ ] Multiple generic parameters with mixed bounds (`fn merge<T: Ord, U: Clone>(...)`)
- [ ] Generic pacts (`pact B<T> { Into<T>; Clone }`)
- [ ] Compile-time monomorphization
- [ ] Constraint alias expansion in type inference

## v0.8.1 — Generic ADTs & Extensions

Parametric polymorphism for types and surface APIs.

- [ ] Generic structs: `struct Pair<T, U> { first: T, second: U }`
- [ ] Generic methods on types: `fn map<V>(…) -> Pair<V, U>`
- [ ] Generic enums: `enum Either<L, R> { Left(L), Right(R) }`
- [ ] Methods on generic enums with self-references: `fn unwrap_left(*e) -> L`
- [ ] Generic type aliases: `type Callback<T> = fn(T) -> bool`
- [ ] Type argument inference at call/instantiation site: `Pair { ... }` or `max(a, b)`

## v0.8.2 — Option & Result

Null safety and error handling.

- [ ] `?T` option type (`Some(v)` / `None`)
- [ ] `T ! E` result type (`Ok(v)` / `Err(e)`)
- [ ] Option/Result pattern matching (`Some(v)`, `Ok(v)`, `Err(e)`)
- [ ] Optional chaining (`?.`)
- [ ] Postfix `!` error propagation
- [ ] Default `None` for `var x: ?T`
- [ ] `if` pattern binding (`if Some(x) := expr { … }`)
- [ ] `while` pattern binding (`while Some(x) := expr { … }`)

## v0.9.0 — Closures, Extensions & Modules

Higher-order programming and code organization.

- [ ] Closures / lambdas (`|x| x * 2`)
- [ ] Closure variable capture
- [ ] Function types (`fn(i32) -> bool`)
- [ ] First-class functions (pass, return, store)
- [ ] Pipe operator (`|>` first-arg forwarding)

## v0.9.1 — Extensions & Module System

- [ ] Extension methods (`ext User { … }`, `ext str { … }`)
- [ ] `use` imports (path, selective, aliased)
- [ ] Private-by-default module resolution
- [ ] `pub` visibility on all declarations (`fn`, `struct`, `enum`, `type`, `var`, `pact`)
- [ ] Generic extension methods: `ext<T: Display> []T { fn join(...) }`
  
## v1.0.0 — Stable Release

Language completeness, tooling, and ecosystem.

- [ ] Language server (LSP) for editor support
- [ ] Compiler diagnostics (clear error messages with source spans)

## v1.0.1 — Core Tooling

- [ ] Formatter (`rsg-fmt`)
- [ ] Linter (`rsg-lint` / static analysis)
- [ ] Test framework (`test` blocks or attribute)

## v1.0.2 — Ecosystem & Runtime

- [ ] Comprehensive standard library (`std/io`, …)
- [ ] `panic(message)` builtin (immediate abort, `noreturn`)
- [ ] `recover()` builtin (catch panic inside `defer`, returns `?str`)
- [ ] `assert` rewritten on top of `panic`
- [ ] Prelude (`panic`, `recover`, `assert`, `Option`, `Result`, etc.), No longer hardcode

## v1.0.3 — Package manager

- [ ] Package manager (dependency resolution, versioning)
- [ ] Unified devtool (`rsg` CLI: build/run/test/lint/fmt integration)

## v1.0.4 — Utilities

- [ ] Benchmark support
- [ ] Documentation generator
- [ ] Full specification freeze

## Future — Post-1.0

- [ ] Debugger & devtool suite (`rsg-debug`, profiler, memory tracker)
- [ ] C++20 back-end
- [ ] Go back-end
- [ ] Typescript back-end
- [ ] Cross-compilation support
- [ ] Compile-time evaluation (`comptime`)
- [ ] Async / concurrency primitives
- [ ] REPL / interpreter mode
- [ ] Playground (web-based)
- [ ] Multi-file compilation
- [ ] Direct C import
- [ ] Self-hosting compiler (Resurg-in-Resurg)
- [ ] Generics: F-bounded polymorphism, `immut`-aware generic constraints
