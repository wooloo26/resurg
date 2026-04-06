# Github Copilot Instructions

**Standard:** C17. Modern idioms only.

## Naming

| Element                | Convention                    | Example               |
| ---------------------- | ----------------------------- | --------------------- |
| Types (struct/enum)    | `PascalCase`                  | `HashTable`           |
| Functions              | `snake_case`, module-prefixed | `hash_table_insert()` |
| Variables              | `snake_case`                  | `item_count`          |
| Constants / Enums      | `UPPER_SNAKE_CASE`            | `MAX_BUFFER_SIZE`     |
| Macros                 | `UPPER_SNAKE_CASE`            | `ARRAY_LEN(a)`        |
| Private functions      | `static snake_case`           | `static parse_header` |
| Globals Vars _(avoid)_ | `g_` prefix                   | `g_config`            |

- Precise verbs (`build`, `compose`, `dispatch`) — not `filter`, `get`, `set`.
- Internal names: explain implementation. Public names: explain functionality.

## File Organization

- `#ifndef`/`#define` guards (no `#pragma once`). Headers expose public API only.
- Include order: own header → `<stdlib>` → `"project/"`.
- Private helpers: `static`.

## Types

- `<stdint.h>` widths (`uint32_t`, `size_t`). `<stdbool.h>` bools.
- Designated initializers: `{ .x = 1, .y = 2 }`. Compound literals for inline structs.
- `_Static_assert` / `static_assert` with message.

## Memory

- Stack/bounded buffers for small data. `snprintf`/`fgets` only (never `sprintf`/`gets`).
- Heap: `Foo *p = malloc(n * sizeof(*p));`. `calloc` only for intentional zero-init.
- Nullify freed pointers that remain in scope.

## Size Limits

Avoid overly long lines, functions, and files.

- **Calls:** break deeply nested or multi-argument calls into named intermediates.
- **Functions / Files:** split only when it genuinely reduces complexity, eliminates duplication, or enforces single responsibility.

## Functions

- `const` for read-only pointer params; `restrict` for non-aliasing contracts.
- Return errors via typed enums, write results to output params:
  ```c
  typedef enum { PARSE_OK, PARSE_ERR_OVERFLOW } ParseResult;
  ParseResult parse_integer(const char *input, int64_t *out_value);
  ```
- `noreturn` from `<stdnoreturn.h>` where appropriate.

## Control Flow

- Always use braces. Declare loop vars in-place: `for (size_t i = 0; ...)`.
- Explicit comparisons for pointers/ints (`ptr != NULL`, `count == 0`). Implicit OK for bools.
- `goto cleanup;` for centralized error handling only.

## Macros

- Use sparingly. Prefer `static inline` functions or `_Generic`.
- Wrap multi-statement macros in `do { } while (0)`.

## Structs

Opaque types in headers (`typedef struct Foo Foo;`). Definitions in `.c` only.

## Banned

- `gets`
- `sprintf`
- `atoi`
- `atof`
- `#pragma once`
- VLAs
- implicit-`int`
- `void main`
- pointer-hiding typedefs
- K&R definitions

## Source Ordering (Bottom-Up)

```
1. Includes & Macros → 2. Static helpers → 3. Mid-level logic → 4. Public API / main
```

## Comments

VSCode-style Doxygen block comments.

- `/** Brief description. */` for single-line doc comments.
- Multi-line doc comments use `/** ... */` blocks with `@` tags (`@file`, `@brief`, `@param`, `@return`, `@p`, `@c`, `@code`/`@endcode`).
- `//` for inline notes, section headers, and non-doc comments.
- `/* buf */` for stretchy-buffer pointers.
- `// consume 'X'` for `advance()` calls.
- `// NOLINTNEXTLINE(check)` for suppression.
