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

- [ ] Struct definition with default field values
- [ ] Struct instantiation and field access
- [ ] Methods with `*` receiver (self)
- [ ] Struct embedding and field/method promotion
- [ ] Method override on embedded types
- [ ] Destructuring (`{name, age} := user`)
- [ ] Tuple destructuring (`(a, b) := pair`)
- [ ] Named arguments at call site

## v0.4.0 — Memory Model

Pointer semantics and garbage collection.

- [ ] `&` heap allocation (returns `*T`)
- [ ] Pointer type `*T` (no arithmetic)
- [ ] Auto-deref on field access (`p.field` on `*T`)
- [ ] Value-type copy semantics on assignment
- [ ] Pointer copy semantics (aliasing)
- [ ] `mut` pointer parameters (declaration + call site)
- [ ] `immut` bindings and fields
- [ ] `immut` return values

## v0.4.1 — Tracing Garbage Collector

- [ ] Tracing GC integration

## v0.5.0 — Enums & Pattern Matching

Algebraic data types and exhaustive matching.

- [ ] Unit enum variants (`Quit`)
- [ ] Tuple enum variants (`Write(str)`)
- [ ] Struct enum variants (`Move { x: i32, y: i32 }`)
- [ ] Enum with explicit discriminants (`Active = 1`)
- [ ] `match` expression (exhaustiveness checking)
- [ ] Literal, range, wildcard, binding patterns
- [ ] Variant patterns (unit, tuple, fields)
- [ ] Match guards (`pattern if cond => ...`)
- [ ] Compile-time rejection of non-exhaustive matches

## v0.6.0 — Option & Result

Null safety and error handling.

- [ ] `?T` option type (`Some(v)` / `None`)
- [ ] `T ! E` result type (`Ok(v)` / `Err(e)`)
- [ ] Option/Result pattern matching (`Some(v)`, `Ok(v)`, `Err(e)`)
- [ ] Optional chaining (`?.`)
- [ ] Postfix `!` error propagation
- [ ] Default `None` for `var x: ?T`

## v0.6.1 — Control Flow, defer & Panic/Recovery

- [ ] `defer` (LIFO cleanup on scope exit)
- [ ] `while` loop
- [ ] `if` pattern binding (`if Some(x) := expr { … }`)
- [ ] `while` pattern binding (`while Some(x) := expr { … }`)
- [ ] `panic(message)` builtin (immediate abort, `noreturn`)
- [ ] `recover()` builtin (catch panic inside `defer`, returns `?str`)
- [ ] `assert` rewritten on top of `panic`

## v0.7.0 — Slices & Collections

Dynamic-length data.

- [ ] Slice type `[]T` (fat pointer: data + length)
- [ ] Array-to-slice conversion (`arr[..]`)
- [ ] Sub-slicing (`s[1..4]`, `s[2..]`, `s[..3]`)
- [ ] Shared backing storage semantics
- [ ] Slice of slices (`[][]T`)
- [ ] `for` over slices with value and index (`|v, i|`)

## v0.8.0 — Pacts & Generics

Abstraction and polymorphism.

- [ ] Generic functions (`fn max<T: Ord>(…)`)
- [ ] Pact-bounded type parameters
- [ ] Generic pacts (`pact B<T> { Into<T>; Clone }`)

## v0.8.0 — Pacts & Conformance

- [ ] Pact (interface) definition with required fields and methods
- [ ] Default method implementations in pacts
- [ ] Explicit conformance (`struct Foo: Pact1 + Pact2`)
- [ ] Constraint aliases (`pact A { Ord; Display } or pact A = Ord + Display`)

## v0.8.1 — Generics Core

- [ ] Generic function syntax (`fn max<T: Ord>(a: T, b: T) -> T`)
- [ ] Pact-bounded type parameters (`T: Ord + Display`)
- [ ] Multiple generic parameters with mixed bounds (`fn merge<T: Ord, U: Clone>(...)`)
- [ ] Generic pacts (`pact B<T> { Into<T>; Clone }`)
- [ ] Compile-time monomorphization
- [ ] Constraint alias expansion in type inference

## v0.8.2 — Generic ADTs & Extensions

Parametric polymorphism for types and surface APIs.

- [ ] Generic structs: `struct Pair<T, U> { first: T, second: U }`
- [ ] Generic methods on types: `fn map<V>(…) -> Pair<V, U>`
- [ ] Generic enums: `enum Either<L, R> { Left(L), Right(R) }`
- [ ] Methods on generic enums with self-references: `fn unwrap_left(*e) -> L`
- [ ] Generic type aliases: `type Callback<T> = fn(T) -> bool`
- [ ] Type argument inference at call/instantiation site: `Pair { ... }` or `max(a, b)`

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
