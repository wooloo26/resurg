# Abbreviations

Common abbreviations used in the resurg compiler codebase and compiler
engineering in general.

## Project Prefix

| Abbr. | Full Form | Notes                                     |
| ----- | --------- | ----------------------------------------- |
| `rsg` | Resurg    | Allowed abbreviation; project-wide prefix |

## Compiler Subsystem Abbreviations

| Abbr.       | Full Form                   | Context                              |
| ----------- | --------------------------- | ------------------------------------ |
| `AST`/`ast` | Abstract Syntax Tree        | Parser output; node/type definitions |
| `TT`/`tt`   | Typed Tree                  | Lowered, fully-typed IR              |
| `IR`/`ir`   | Intermediate Representation | General compiler term                |
| `sema`      | Semantic Analysis           | Type checking and scope resolution   |
| `cgen`      | Code Generator/Generation   | backend emission                   |

## Token & Syntax Abbreviations

| Abbr.   | Full Form   | Context                                    |
| ------- | ----------- | ------------------------------------------ |
| `op`    | Operator    | `TokenKind` in unary/binary/compound nodes |
| `decl`  | Declaration | Variable/function/struct/enum declarations |
| `expr`  | Expression  | AST/TT expression nodes                    |
| `stmt`  | Statement   | AST/TT statement nodes                     |
| `param` | Parameter   | Function parameter nodes                   |
| `arg`   | Argument    | Call-site argument nodes                   |
| `lit`   | Literal     | Literal value nodes                        |
| `id`    | Identifier  | Name references                            |

## Type System Abbreviations

| Abbr.   | Full Form                      | Context                |
| ------- | ------------------------------ | ---------------------- |
| `i8`    | 8-bit signed integer           | Language type keyword  |
| `i16`   | 16-bit signed integer          | Language type keyword  |
| `i32`   | 32-bit signed integer          | Language type keyword  |
| `i64`   | 64-bit signed integer          | Language type keyword  |
| `i128`  | 128-bit signed integer         | Language type keyword  |
| `u8`    | 8-bit unsigned integer         | Language type keyword  |
| `u16`   | 16-bit unsigned integer        | Language type keyword  |
| `u32`   | 32-bit unsigned integer        | Language type keyword  |
| `u64`   | 64-bit unsigned integer        | Language type keyword  |
| `u128`  | 128-bit unsigned integer       | Language type keyword  |
| `isize` | Pointer-sized signed integer   | Language type keyword  |
| `usize` | Pointer-sized unsigned integer | Language type keyword  |
| `f32`   | 32-bit floating point          | Language type keyword  |
| `f64`   | 64-bit floating point          | Language type keyword  |
| `str`   | String                         | Language type keyword  |
| `mut`   | Mutable                        | Mutability qualifier   |
| `immut` | Immutable                      | Immutability qualifier |
| `pub`   | Public                         | Visibility qualifier   |
| `fn`    | Function                       | Language keyword       |

## Data Structure Abbreviations

| Abbr. | Full Form | Context                        |
| ----- | --------- | ------------------------------ |
| `buf` | Buffer    | Stretchy-buffer pointer marker |
| `sym` | Symbol    | Scope symbol / `TtSymbol`      |
| `ref` | Reference | Variable reference nodes       |

## Common Variable Name Abbreviations

| Abbr.   | Full Form      | Usage Context                       |
| ------- | -------------- | ----------------------------------- |
| `loc`   | Location       | `SourceLocation` values             |
| `idx`   | Index          | Array/element index                 |
| `pos`   | Position       | Parser position / offset            |
| `tmp`   | Temporary      | Generated temporary variable names  |
| `init`  | Initializer    | Variable initialization expressions |
| `cond`  | Condition      | If/loop condition nodes             |
| `ctx`   | Context        | Visitor/callback context pointer    |
| `elem`  | Element        | Array/tuple element                 |
| `alloc` | Allocation     | Memory allocation                   |
| `ptr`   | Pointer        | Pointer value                       |
| `msg`   | Message        | Error/assert message                |
| `fmt`   | Format         | Format string                       |
| `sb`    | String Builder | `RsgStringBuilder` instances        |
| `recv`  | Receiver       | Method receiver parameter           |
| `len`   | Length         | Text/buffer length                  |
| `deref` | Dereference    | Pointer dereference nodes           |

## Build & Tooling Abbreviations

| Abbr. | Full Form                | Context                   |
| ----- | ------------------------ | ------------------------- |
| `lsp` | Language Server Protocol | `rsg-lsp` tool            |
| `pkg` | Package                  | `rsg-pkg` package manager |
