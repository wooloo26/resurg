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

TT references Sema's `Type` directly — no parallel type system.

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
```

- **Arenas:** Monomorphised types and TT nodes use Lowering-owned arenas (not Sema's), freeable independently.
- **Invariant:** Every new `TypeKind` variant **must** have a `type_equal()` branch.
- **Accessors:** All variant member access goes through accessor functions (`type_array_element()`, `type_pointee()`, etc.) — never direct field access.

### Parametric Type Utilities

```pseudo
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

---

## 2. Symbol Table (`TtSymbol`)

```pseudo
struct TtSymbol {
    kind:         TtSymbolKind   // SYM_VAR | SYM_PARAM | SYM_FUNCTION | SYM_TYPE
                                 // SYM_FIELD | SYM_VARIANT | SYM_MODULE
    sema_symbol:  *Symbol
    is_immut:     bool
    mangled_name: string         // nullable, filled by lowering
    owner:        *TtSymbol      // enclosing struct/enum/module (nullable)
    capture_mode: CaptureMode    // CAPTURE_NONE | BY_VALUE | BY_REF
    location:     SourceLocation
}
```

`TtSymbol.kind` is authoritative in TT/codegen. Accessors delegate to Sema: `tt_symbol_name(sym)` → `sym.sema_symbol.name`.

**Prerequisite:** Sema's `Symbol` needs `SymbolKind` enum, `is_immut`, `declaration`, and `owner` fields. See [TYPED_TREE_IMPL.md](TYPED_TREE_IMPL.md) §2.

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

All TT nodes embed `TtNodeHeader` as first member (tagged union). Non-expression nodes have `header.type = TYPE_UNIT`.

### 3.1 Declarations

```pseudo
struct TtModule         { header; name; declarations: []*TtNode }
struct TtStructDecl     { header; name; is_public; type_params; fields: []*TtFieldDecl;
                          methods: []*TtFunctionDecl; embeddings: []*Type;
                          conformances: []*TtPactDecl }
struct TtFieldDecl      { name; type: *Type; default_value: *TtExpr; is_immut }
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
struct TtParam          { symbol: *TtSymbol; name; type: *Type; is_mut }
struct TtExtensionDecl  { header; target_type: *Type; type_params;
                          methods: []*TtFunctionDecl }
```

- `target_type` may contain `TYPE_TYPE_PARAM` for partially-parameterised extensions.
- Embedding conflict: innermost wins; same-depth ambiguity is a lowering error.

### 3.2 Statements

```pseudo
struct TtVarDecl   { header; symbol; name; type; initializer: *TtExpr; is_immut }
struct TtDefer     { header; body: *TtBlock }
struct TtReturn    { header; value: *TtExpr }
struct TtAssign    { header; target: *TtExpr; value: *TtExpr }
struct TtBreak     { header; value: *TtExpr }   // nullable; TYPE_UNIT when absent
struct TtContinue  { header }                    // header.type = TYPE_UNIT
```

All loop forms desugar into `TtLoop` using `TtBreak`/`TtContinue` for control flow (§4).

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
```

```pseudo
struct TtIntLit       { header; value: u64 }
struct TtVarRef       { header; symbol: *TtSymbol }
struct TtFieldAccess  { header; object: *TtExpr; field: *TtSymbol }
struct TtCall         { header; callee: *TtExpr; arguments: []*TtCallArg;
                        type_args: []*Type;
                        dispatch_kind: STATIC|DYNAMIC; receiver: *TtExpr }
struct TtCallArg      { name; value: *TtExpr; is_mut }
struct TtClosure      { header; params; body: *TtExpr; captures: []*TtSymbol }
struct TtIf           { header; condition; then_body; else_body }
struct TtBlock        { header; statements: []*TtNode; result: *TtExpr }
struct TtPropagate    { header; inner: *TtExpr;
                        propagation_kind: PROPAGATE_RESULT | PROPAGATE_OPTION;
                        err_type: *Type }
struct TtHeapAlloc    { header; inner: *TtExpr }
struct TtLoop         { header; body: *TtBlock }
```

- `TtIf` handles boolean conditions only; if-let desugars to `TtMatchExpr`.
- `TtBlock.result`: tail expression for expression-blocks (not in `statements`). NULL for statement-blocks (`header.type = TYPE_UNIT`).
- `TtPropagate.header.type` = enclosing function/closure's return type. Lowering maintains a return-type stack for closures.

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

Side-effect targets in compound assignments bind to a temporary. Range ends also bind to temporaries.

---

## 5. Back-End Mapping

### Type Mapping

| Type     | C17                  | C++20              | Go                  | TypeScript    |
| -------- | -------------------- | ------------------ | ------------------- | ------------- |
| `BOOL`   | `bool`               | `bool`             | `bool`              | `boolean`     |
| `I32`    | `int32_t`            | `std::int32_t`     | `int32`             | `number`      |
| `I64`    | `int64_t`            | `std::int64_t`     | `int64`             | `bigint`      |
| `F64`    | `double`             | `double`           | `float64`           | `number`      |
| `CHAR`   | `uint32_t`           | `char32_t`         | `rune`              | `string`      |
| `STRING` | `RsgString`          | `std::string`      | `string`            | `string`      |
| `UNIT`   | `void`               | `void`             | `struct{}`          | `void`        |
| `[N]T`   | `T arr[N]`           | `std::array<T,N>`  | `[N]T`              | `T[]`         |
| `[]T`    | `RsgSlice_T`         | `std::span<T>`     | `[]T`               | `T[]`         |
| `(A,B)`  | `struct{A _0;B _1;}` | `std::tuple<A,B>`  | `struct{_0 A;_1 B}` | `[A, B]`      |
| `*T`     | `T*`                 | `T*`/`shared_ptr`  | `*T`                | `T`           |
| `?T`     | `RsgOption_T`        | `std::optional<T>` | `*T`/nil            | `T \| null`   |
| `T!E`    | `RsgResult_T_E`      | `std::expected`    | `(T, error)`        | `Result<T,E>` |
| `fn→R`   | function pointer     | `std::function`    | `func(…) R`         | `(…) => R`    |

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

---

## 6. Example: End-to-End Lowering

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
    result → VarRef(y)
```

---

## 7. Design Invariants

1. **No NULL types.** `TYPE_ERROR` for poison.
2. **No unresolved names.** Every identifier → `TtVarRef` → `TtSymbol` → `Symbol`.
3. **No syntactic sugar.** All desugaring in lowering.
4. **Exhaustive patterns.** Guaranteed by Sema.
5. **Monomorphised generics.** Each instantiation = separate declaration, deduplicated by mangled name.
6. **Immutability tracked.** `TtSymbol.is_immut` resolved; back-ends emit `const`/`readonly`.
7. **Back-end independence.** No target-specific info in TT nodes.
8. **Source locations preserved.** For diagnostics from TT Passes.
9. **`recover()` is a builtin.** → `TtCall { callee=builtin_recover }` → `?str`. Valid only inside `TtDefer`; Sema enforces.
10. **Return type wrapping.** Lowering inserts `Ok(…)`/`Some(…)` so `TtReturn.value.type` always equals `TtFunctionDecl.return_type`.
11. **Destructuring desugared.** → individual `TtVarDecl` + `TtFieldAccess`/`TtTupleIndex`.
12. **Binary op type consistency.** `left.type == right.type == header.type` for numeric ops.
13. **Closure scoping.** `TtReturn` and `TtPropagate` in closures use the closure's return type, not the enclosing function's.
14. **`TtBlock.result` separate from `statements`.** Expression-blocks: tail in `result`. Statement-blocks: `result` is NULL.
15. **`TtBreak`/`TtContinue` required in `TtLoop`.** All loop forms desugar into `TtLoop` with explicit break/continue.
16. **TT trusts Sema.** No re-validation of type compatibility, exhaustiveness, or scope validity.

---

## 8. Mangled Name Convention

`tt_mangle_name()` — deterministic, injective, testable.

**Format:** `{module}_{name}_{type1}_{type2}_{...}`

| Type            | Mangled form             | Example                  |
| --------------- | ------------------------ | ------------------------ |
| Primitives      | lowercase kind           | `i32`, `f64`, `str`      |
| `[N]T`          | `arr{N}_{T}`             | `arr3_i32`               |
| `[]T`           | `slice_{T}`              | `slice_u8`               |
| `(A, B)`        | `tup_{A}_{B}`            | `tup_i32_str`            |
| `*T` / `*mut T` | `ptr_{T}` / `mutptr_{T}` | `ptr_i32` / `mutptr_i32` |
| `?T`            | `opt_{T}`                | `opt_str`                |
| `T ! E`         | `res_{T}_{E}`            | `res_f32_str`            |
| `fn(A,B)->R`    | `fn_{A}_{B}_to_{R}`      | `fn_i32_str_to_bool`     |
| Nominal         | declaration name         | `Vec`, `HashMap`         |
| Nested generics | recursive                | `Pair_i32_Pair_i32_str`  |

**Reserved prefixes** (`arr`, `slice`, `tup`, `ptr`, `mutptr`, `opt`, `res`, `fn`) — user types must not use these names. `_to_` separates params from return type in function mangling.

Cross-module: mangled names are module-qualified; same instantiation emitted once.
