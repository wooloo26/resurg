# Prompts

## vx.x.x (tag: vx.x.x)

Implement `#version` via specification-driven TDD: 1) author test cases strictly from `README.md & ROADMAP.md` (zero source code reference), 2) rigorously review and validate the test contract to lock down expected behavior, 3) implement the feature to satisfy the approved tests, and 4) finalize with make lint && make test.

## deduplicate code and simplify signatures

Refactor: Deduplicate code. Convert functions with >4 params (lacking justification) to structs. Run `make lint & make test`. Loop ×3: ANALYZE → APPLY → VALIDATE.

## split large files, long functions and flatten call chains

Refactor: Break down oversized files, long functions, deep call chains. Run `make lint & make test`. Loop ×3: ANALYZE → APPLY → VALIDATE.

## enforce SRP, OCP, and ISP principles

Refactor: Enforce SRP, OCP, ISP. Avoid over-engineering. Run `make lint & make test`. Loop ×3: ANALYZE → APPLY → VALIDATE.

## favor composition and apply DIP

Refactor: Favor composition. Ensure high cohesion & low coupling. Apply DIP. Avoid over-engineering. Run `make lint & make test`. Loop ×3: ANALYZE → APPLY → VALIDATE.

## clarify naming and extract pure functions

Refactor: Sharpen naming accuracy. Extract pure functions. Run `make lint & make test`. Loop ×3: ANALYZE → APPLY → VALIDATE.
