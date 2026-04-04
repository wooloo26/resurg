# Resurg Typed Tree

## Overview

```
Source (.rsg)
  → Lexer ─── tokens
  → Parser ── AST (untyped)
  → Sema ──── AST + types + symbols + errors
  → Lowering (AST → TT)
      desugar, bind symbols, normalize method calls,
      build compound type registry, monomorphize
  → TT Passes (TT → TT, optional, composable)
      1. Constant folding & dead branch elimination
      2. Simplification (ternary, inline, tail-return)
      3. Control flow analysis
      4. Mutability validation
      5. Closure capture analysis
  → CodeGen (type→string, TtNode→syntax)
      C17 | C++ | Go | TypeScript
```

Every TT node carries a **resolved type** (never NULL), **desugared** constructs, **scope-resolved identifiers** (`TtSymbol` wrapping Sema's `Symbol`), and **monomorphised generics**.

---

## 1. Types — Reuse Sema's Type

TT references Sema's `Type` directly — no parallel type system. Monomorphised types use Lowering-owned arenas. Access variant members through accessors (`type_array_element()`, etc.) — never direct field access. Every `TypeKind` must have a `type_equal()` branch.

```pseudo
struct Type {
    kind: TypeKind
    array:      { element: *Type; size: i32 }
    tuple:      { elements: []*Type; count: i32 }
    pointer:    { pointee: *Type; is_mut: bool }
    slice:      { element: *Type }
    option:     { inner: *Type }
    result:     { ok_type: *Type; err_type: *Type }
    function:   { params: []*Type; ret: *Type; is_closure: bool }
    nominal:    { decl: *Decl }              // struct/enum/pact
    type_param: { name: string }
}

type_contains_param(type: *Type) -> bool
type_substitute(arena: *Arena, type: *Type, param_name: string, concrete: *Type) -> *Type
type_substitute_all(arena: *Arena, type: *Type, param_names: []string,
                    concretes: []*Type, count: i32) -> *Type
```

### Type Identity

| Kind          | Rule                                   |
| ------------- | -------------------------------------- |
| Primitives    | pointer equality (singletons)          |
| `[N]T`        | N equal and T equal                    |
| `[]T`, `?T`   | element equal                          |
| `(A, B)`      | pairwise equality                      |
| `*T`          | pointee equal (mut checked separately) |
| `T ! E`       | structural                             |
| Struct / Enum | nominal (same `Decl*`)                 |
| `fn(…)->R`    | param-wise + return type equal         |
| `type_param`  | name equality (pre-mono only)          |

After monomorphisation, no `type_param` should remain. `type_contains_param()` returning `true` post-mono is a bug.

---

## 2. Symbol Table (`TtSymbol`)

```pseudo
struct TtSymbol {
    kind:         TtSymbolKind   // SYM_VAR | SYM_PARAM | SYM_FUNCTION | SYM_TYPE
                                 // SYM_FIELD | SYM_VARIANT | SYM_MODULE
    sema_symbol:  *Symbol
    is_mut:       bool           // binding mutability: var/:= (true) vs immut (false)
    mangled_name: string         // nullable, filled by lowering
    owner:        *TtSymbol      // enclosing struct/enum/module (nullable)
    capture_mode: CaptureMode    // CAPTURE_NONE | BY_VALUE | BY_REF
    location:     SourceLocation
}
```

`TtSymbol.kind` is authoritative in TT/codegen. Accessors delegate to Sema: `tt_symbol_name(sym)` → `sym.sema_symbol.name`.

**Two mutability axes** (independent despite sharing `is_mut` name):
- **Binding** — reassignability. `TtSymbol.is_mut`, `TtVarDecl.is_mut`, `TtFieldDecl.is_mut`. `var`/`:=` vs `immut`.
- **Pointer** — pointee mutation. `TtParam.is_mut`, `TtCallArg.pass_mode`, `Type.pointer.is_mut`. `*T` vs `*mut T`.

---

## 3. Node Hierarchy

### Common Header

```pseudo
struct TtNodeHeader {
    kind:     TtNodeKind
    type:     *Type            // never NULL
    location: SourceLocation
}
```

All TT nodes embed `TtNodeHeader` as first member (tagged union). Non-expression nodes: `header.type = TYPE_UNIT`.

### 3.1 Declarations

```pseudo
struct TtModule         { header; name; declarations: []*TtNode }
struct TtStructDecl     { header; name; is_public; type_params; fields: []*TtFieldDecl;
                          methods: []*TtFunctionDecl; embeddings: []*Type;
                          conformances: []*TtPactDecl }
struct TtFieldDecl      { name; type: *Type; default_value: *TtExpr; is_mut }  // binding mut
struct TtEnumDecl       { header; name; is_public; type_params;
                          variants: []*TtVariantDecl; methods: []*TtFunctionDecl }
struct TtVariantDecl    { name; kind: UNIT|TUPLE|STRUCT;
                          tuple_types; fields; discriminant: *TtExpr }
struct TtPactDecl       { header; name; type_params; required_fields; required_methods;
                          default_methods; super_pacts }
struct TtTypeAlias      { header; name; is_public; type_params; underlying: *Type }
struct TtFunctionDecl   { header; name; is_public; type_params;
                          receiver: *TtParam; params: []*TtParam;
                          return_type: *Type; body: *TtBlock }
struct TtParam          { symbol: *TtSymbol; name; type: *Type; is_mut }  // pointer mut
struct TtExtensionDecl  { header; target_type: *Type; type_params;
                          methods: []*TtFunctionDecl }
```

**Default method dispatch:** Default pact methods are monomorphised into each conforming type (Rust-style), substituting `Self`. `TtCall.dispatch_kind`: `STATIC` | `DYNAMIC` (vtable).

**Extensions:** Lowering maps base type → `TtExtensionDecl` list. Struct's own method wins. Duplicate extension methods for the same target is a lowering error. Parameterised extensions monomorphise alongside their target.

**Embedding resolution:** Promoted fields/methods resolved by shortest embedding chain depth (DFS). Shallowest wins; same-depth ambiguity → error. Diamond with same source type → deduplicated; different source types → error.

### 3.2 Statements

```pseudo
struct TtVarDecl   { header; symbol; name; type; initializer: *TtExpr; is_mut }  // binding mut
struct TtDefer     { header; body: *TtBlock }
struct TtReturn    { header; value: *TtExpr }
struct TtAssign    { header; target: *TtExpr; value: *TtExpr }
struct TtBreak     { header; value: *TtExpr }   // nullable; TYPE_UNIT when absent
struct TtContinue  { header }                    // header.type = TYPE_UNIT
```

### 3.3 Expressions

```pseudo
enum TtExprKind {
    // Literals: BOOL_LIT, INT_LIT, FLOAT_LIT, CHAR_LIT, STRING_LIT,
    //           UNIT_LIT, ARRAY_LIT, TUPLE_LIT, STRUCT_LIT
    // References: VAR_REF, FIELD_ACCESS, TUPLE_INDEX, INDEX, MODULE_ACCESS
    // Operations: UNARY, BINARY, CALL, TYPE_CONV
    // Constructors: SOME, NONE, OK, ERR, ENUM_VARIANT, HEAP_ALLOC
    // Control flow: IF, MATCH, BLOCK, LOOP
    // Special: CLOSURE, PROPAGATE, PANIC, RECOVER
}

struct TtIntLit       { header; value: u64 }
struct TtVarRef       { header; symbol: *TtSymbol }
struct TtFieldAccess  { header; object: *TtExpr; field: *TtSymbol }
struct TtCall         { header; callee: *TtExpr; arguments: []*TtCallArg;
                        type_args: []*Type;
                        dispatch_kind: STATIC|DYNAMIC; receiver: *TtExpr }
struct TtCallArg      { name; value: *TtExpr; pass_mode: BY_VALUE|BY_REF|BY_MUT_REF }
struct TtTypeConv     { header; operand: *TtExpr; target_type: *Type }
struct TtClosure      { header; params; body: *TtExpr; captures: []*TtSymbol }
struct TtIf           { header; condition; then_body; else_body }
struct TtBlock        { header; statements: []*TtNode; result: *TtExpr }
struct TtPropagate    { header; inner: *TtExpr;
                        propagation_kind: PROPAGATE_RESULT | PROPAGATE_OPTION;
                        err_type: *Type }
struct TtHeapAlloc    { header; inner: *TtExpr }
struct TtLoop         { header; body: *TtBlock }
```

- `TtIf`: boolean only; if-let → `TtMatchExpr`.
- `TtBlock.result`: tail expression (not in `statements`); NULL for statement-blocks.
- `TtPropagate.header.type` = enclosing function/closure return type. Forbidden inside `TtDefer`.
- `TtTypeConv`: explicit cast (`i64(100)`). Safe widenings and explicit narrowings only.

### 3.4 Patterns

```pseudo
enum TtPatternKind {
    PAT_WILDCARD, PAT_BINDING, PAT_LITERAL, PAT_RANGE,
    PAT_VARIANT_UNIT, PAT_VARIANT_TUPLE, PAT_VARIANT_STRUCT,
    PAT_SOME, PAT_NONE, PAT_OK, PAT_ERR, PAT_TUPLE, PAT_STRUCT,
}

struct TtPattern {
    kind; type: *Type; location;
    binding:  { symbol: *TtSymbol }
    literal:  { value: *TtExpr }
    range:    { start; end; inclusive }
    variant:  { decl: *TtVariantDecl; sub_patterns }
    option:   { inner: *TtPattern }
    result:   { inner: *TtPattern }
    tuple:    { elements: []*TtPattern }
}

struct TtMatchExpr { header; scrutinee: *TtExpr; arms: []*TtMatchArm }
struct TtMatchArm  { pattern; guard: *TtExpr; body: *TtExpr }
```

---

## 4. Desugaring Rules

| Source                        | TT form                                                        |
| ----------------------------- | -------------------------------------------------------------- |
| `x := expr`                   | `TtVarDecl { type=inferred, init=expr }`                       |
| `x += expr`                   | `TtAssign { target=x, value=TtBinary(ADD, x, expr) }`          |
| `"hello {name}"`              | `TtCall { callee=rsg_string_build, args=[…] }`                 |
| `?T` / `T ! E`                | `TYPE_OPTION` / `TYPE_RESULT`                                  |
| `expr!` (on `T!E`)            | `TtPropagate { inner=expr, kind=RESULT }`                      |
| `expr!` (on `?T`)             | `TtPropagate { inner=expr, kind=OPTION }`                      |
| `expr?.field`                 | `TtMatch { Some(v)→v.field, None→None }`                       |
| `&Struct{…}`                  | `TtHeapAlloc { inner=TtStructLit }`                            |
| `user.method(a)`              | `TtCall { callee=method_sym, args=[user, a] }`                 |
| `a \|> f`                     | `TtCall { callee=f, args=[a] }`                                |
| `while cond { body }`         | `TtLoop { if !cond break; body }`                              |
| `while Some(x) := e { body }` | `TtLoop { tmp:=e; TtMatch(tmp) { Some(x)→body, None→break } }` |
| `for 0..n \|i\| body`         | `TtLoop { var _end=n; var i=0; if i>=_end break; body; i+=1 }` |
| `if Some(x) := e { … }`       | `TtMatch(e) { Some(x)→then, _→else }`                          |
| `{name,age} := user`          | individual `TtVarDecl` + `TtFieldAccess` per field             |
| `(a, b) := pair`              | individual `TtVarDecl` + `TtTupleIndex` per element            |
| embedding                     | promoted fields/methods copied into `TtStructDecl`             |
| generic `fn max<T: Ord>`      | monomorphised: `max_i32`, `max_f64`, …                         |

Compound assignment side-effect targets and range ends bind to temporaries.

---

## 5. Back-End Mapping

### Type Mapping

| Type     | C17                  | C++20               | Go                  | TypeScript        |
| -------- | -------------------- | ------------------- | ------------------- | ----------------- |
| `BOOL`   | `bool`               | `bool`              | `bool`              | `boolean`         |
| `I8–I64` | `int8_t`–`int64_t`   | `std::int8_t`–      | `int8`–`int64`      | `number`/`bigint` |
| `I128`   | `__int128`           | `__int128`          | `big.Int`           | `bigint`          |
| `U8–U64` | `uint8_t`–`uint64_t` | `std::uint8_t`–     | `uint8`–`uint64`    | `number`/`bigint` |
| `U128`   | `unsigned __int128`  | `unsigned __int128` | `big.Int`           | `bigint`          |
| `ISIZE`  | `intptr_t`           | `std::intptr_t`     | `int`               | `number`          |
| `USIZE`  | `size_t`             | `std::size_t`       | `uint`              | `number`          |
| `F32/64` | `float`/`double`     | `float`/`double`    | `float32`/`float64` | `number`          |
| `CHAR`   | `uint32_t`           | `char32_t`          | `rune`              | `string`          |
| `STRING` | `RsgString`          | `std::string`       | `string`            | `string`          |
| `UNIT`   | `void`               | `void`              | `struct{}`          | `void`            |
| `[N]T`   | `T arr[N]`           | `std::array<T,N>`   | `[N]T`              | `T[]`             |
| `[]T`    | `RsgSlice_T`         | `std::span<T>`      | `[]T`               | `T[]`             |
| `(A,B)`  | `struct{A _0;B _1;}` | `std::tuple<A,B>`   | `struct{_0 A;_1 B}` | `[A, B]`          |
| `*T`     | `T*`                 | `T*`/`shared_ptr`   | `*T`                | `T`               |
| `?T`     | `RsgOption_T`        | `std::optional<T>`  | `*T`/nil            | `T \| null`       |
| `T!E`    | `RsgResult_T_E`      | `std::expected`     | `(T, error)`        | `Result<T,E>`     |
| `fn→R`   | function pointer     | `std::function`     | `func(…) R`         | `(…) => R`        |

`CHAR` = Unicode scalar value (32-bit), not C's `char`.

### Construct Mapping

| TT Node        | C17                    | C++20                   | Go                | TypeScript          |
| -------------- | ---------------------- | ----------------------- | ----------------- | ------------------- |
| `TtStructDecl` | `typedef struct`       | `class`/`struct`        | `type … struct`   | `class`             |
| `TtEnumDecl`   | tagged union           | `std::variant`          | interface + types | discriminated union |
| `TtPactDecl`   | vtable struct          | `concept`/abstract base | `interface`       | `interface`         |
| `TtMatchExpr`  | `switch`+`if`          | `std::visit`/`switch`   | `switch v.(type)` | `switch(tag)`       |
| `TtDefer`      | `goto cleanup`         | RAII/scope guard        | `defer`           | `try/finally`       |
| `TtPropagate`  | `if (is_err) return`   | `TRY()` macro           | `if err != nil`   | `throw`             |
| `TtClosure`    | struct + fn pointer    | lambda                  | closure literal   | arrow function      |
| `TtHeapAlloc`  | `malloc` + GC register | `make_shared`           | `&T{}`            | `new T()`           |
| `TtPanic`      | `fprintf + abort`      | `std::terminate`        | `panic()`         | `throw new Error()` |
| `TtRecover`    | `setjmp`/`longjmp`     | `try`/`catch`           | `recover()`       | `try`/`catch`       |

#### C17 `setjmp`/`longjmp` Rules

`TtPanic` emits defer cleanup inline before `longjmp`. Locals between `setjmp`/`longjmp` marked `volatile`. No `restrict` in functions containing `setjmp`. Other backends use native exceptions.

---

## 6. Example

```rsg
fn divide(a: f32, b: f32) -> f32 ! str {
    if b == 0.0 { Err("division by zero") }
    else { Ok(a / b) }
}
fn calc() -> f32 ! str {
    x := divide(10.0, 2.0)!
    y := divide(x, 5.0)!
    y
}
```

```
TtFunctionDecl "divide"
  params: [a: F32, b: F32], return: RESULT(F32, STRING)
  body → TtIf
    cond: Binary(EQ, VarRef(b), FloatLit(0.0))
    then → Err(StringLit("division by zero"))
    else → Ok(Binary(DIV, VarRef(a), VarRef(b)))

TtFunctionDecl "calc"
  return: RESULT(F32, STRING)
  body:
    VarDecl x:F32 = Propagate(Call(divide, [10.0, 2.0]), err=STRING)
    VarDecl y:F32 = Propagate(Call(divide, [x, 5.0]), err=STRING)
    result → Ok(VarRef(y))
```

---

## 7. Design Invariants

1. **No NULL types.** `TYPE_ERROR` for poison.
2. **No unresolved names.** Every identifier → `TtVarRef` → `TtSymbol` → `Symbol`.
3. **No syntactic sugar.** All desugaring in lowering.
4. **Exhaustive patterns.** Guaranteed by Sema.
5. **Monomorphised generics.** Deduplicated by mangled name.
6. **Mutability tracked.** Binding + pointer mutability fully resolved.
7. **Back-end independence.** No target-specific info in TT nodes.
8. **Source locations preserved.** For diagnostics from TT Passes.
9. **`recover()` builtin** → `TtCall { callee=builtin_recover }` → `?str`. Only in `TtDefer`.
10. **Return type wrapping.** Lowering inserts `Ok(…)`/`Some(…)` so return values match declared type.
11. **Destructuring desugared** → individual `TtVarDecl` + `TtFieldAccess`/`TtTupleIndex`.
12. **Binary op type consistency.** `left.type == right.type == header.type` for numeric ops.
13. **Closure scoping.** `TtReturn`/`TtPropagate` use closure's return type, not enclosing function's.
14. **`TtBlock.result` separate from `statements`.** NULL for statement-blocks.
15. **All loops → `TtLoop`** with explicit `TtBreak`/`TtContinue`.
16. **TT trusts Sema.** No re-validation.

### Lowering Error Model

- Accumulate errors, emit partial TT. Poison nodes (`TYPE_ERROR`) for unlowerable constructs.
- Fatal threshold: 50 errors (configurable). Diagnostics via `rsg_error()`.

### Constant Folding

- **Integer overflow:** wrapping (unsigned) / two's complement (signed).
- **Float:** IEEE 754. `0.0 / 0.0` → `NaN`.
- **Dead branches:** constant `TtBoolLit` condition → eliminated.

---

## 8. Mangled Name Convention

`tt_mangle_name()` — deterministic, injective. Format: `_R{module_seg}_{name_seg}_{type_params}` (`_R` prefix reserved). Length-prefixed segments (Itanium-inspired).

| Type            | Mangled       | Example                         |
| --------------- | ------------- | ------------------------------- |
| Primitives      | short tag     | `b`, `i32`, `f64`, `s`          |
| `[N]T`          | `A{N}{T}`     | `A3i32` → `[3]i32`              |
| `[]T`           | `S{T}`        | `Su8` → `[]u8`                  |
| `(A, B)`        | `T{n}{A}{B}`  | `T2i32s` → `(i32, str)`         |
| `*T` / `*mut T` | `P{T}`/`M{T}` | `Pi32` / `Mi32`                 |
| `?T`            | `O{T}`        | `Os` → `?str`                   |
| `T ! E`         | `E{T}{E}`     | `Ef32s` → `f32 ! str`           |
| `fn(A,B)->R`    | `F{n}{…}{R}`  | `F2i32sb` → `fn(i32,str)->bool` |
| Nominal         | `{len}{name}` | `3Vec`, `7HashMap`              |

```
_R4math_3max_i32     // module math, fn max<i32>
_R3net_4Conn         // module net, struct Conn
```

**Deduplication:** Keyed by mangled name using defining module. `Vec<i32>` from modules A and B → `_R3std_3Vec_i32`, emitted once.
