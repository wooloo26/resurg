# Macro & Comptime

```text
Source (.rsg)
  ↓
[ pass/lex ]        → Tokens
  ↓
[ pass/parse ]      → Untyped AST
  ↓
[ pass/macro_expand]→ ✨ MACRO EXPANSION ✨
  ↓
[ pass/resolve ]    → AST + Symbol Bindings
  ↓
[ pass/infer ]      → Type Equations
  ↓
[ pass/check ]      → Fully Typed AST
  ↓
[ pass/comptime ]   → ⚡ COMPTIME EVAL ⚡
  ↓
[ pass/mono ]       → Monomorphized AST
  ↓
[ pass/lower ]      → HIR
  ↓
[ pass/cgen ]       → Target Code
```
