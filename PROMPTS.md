# Prompts

## Per Version Cycle

1. Implement `#version` via specification-driven TDD: 1) author test cases strictly from `README.md & ROADMAP.md` (zero source code reference), 2) rigorously review and validate the test contract to lock down expected behavior, 3) implement the feature to satisfy the approved tests while strictly adhering to `TYPED_TREE.md`, and 4) finalize with make lint && make test.
    - vx.x.x (tag: vx.x.x)
2. Refactor: Deduplicate code. Convert functions with >4 params (lacking justification) to structs. Run `make lint & make test`. Loop ×3: ANALYZE → APPLY → VALIDATE.
    - deduplicate code and simplify signatures
3. Refactor: Break down oversized files, long functions, deep call chains. Run `make lint & make test`. Loop ×3: ANALYZE → APPLY → VALIDATE.
    - split large files, long functions and flatten call chains

## Every 2nd Version

1. Refactor: Enforce SRP, OCP, ISP. Avoid over-engineering. Run `make lint & make test`. Loop ×3: ANALYZE → APPLY → VALIDATE.
    - enforce SRP, OCP, and ISP principles
2. Refactor: Favor composition. Ensure high cohesion & low coupling. Apply DIP. Avoid over-engineering. Run `make lint & make test`. Loop ×3: ANALYZE → APPLY → VALIDATE.
    - favor composition and apply DIP
3. Refactor: Sharpen naming accuracy. Extract pure functions. Run `make lint & make test`. Loop ×3: ANALYZE → APPLY → VALIDATE.
    - clarify naming and extract pure functions
4. Refactor any non-compliance with TYPED_TREE.md; do not implement features not already present.
