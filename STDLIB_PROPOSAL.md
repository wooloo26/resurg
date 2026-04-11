# Standard Library Proposal (v0.9.13)

Comprehensive recommendation for built-in functions, primitive extension methods,
and standard library modules.

---

## 1. Built-in Functions (Compiler Intrinsics)

These require special compiler support (type dispatch, source-location injection, etc.)
and are declared in `std/builtin.rsg`.

### 1.1 Currently Implemented

| Function      | Signature                                  |
| ------------- | ------------------------------------------ |
| `print`       | `fn print<T>(value: T)`                    |
| `println`     | `fn println<T>(value: T)`                  |
| `assert`      | `fn assert(cond: bool, msg: str)`          |
| `len`         | `fn len<T>(value: T) -> i32`               |
| `panic`       | `fn panic(msg: str) -> never`              |
| `catch_panic` | `fn catch_panic<T>(f: Fn() -> T) -> T!str` |

### 1.2 Proposed Additions

#### Type Conversion & Inspection

| Function    | Signature                       | Description                                                                |
| ----------- | ------------------------------- | -------------------------------------------------------------------------- |
| `sizeof`    | `fn sizeof<T>() -> usize`       | Compile-time size in bytes. Codegen emits `sizeof(CType)`.                 |
| `alignof`   | `fn alignof<T>() -> usize`      | Compile-time alignment. Codegen emits `_Alignof(CType)`.                   |
| `default`   | `fn default<T: Default>() -> T` | Zero/default value for a type. Used by generic code to get initial values. |
| `type_name` | `fn type_name<T>() -> str`      | Compile-time string of type name (debugging aid).                          |

#### String-to-Number Parsing

| Function     | Signature                             | Description                                        |
| ------------ | ------------------------------------- | -------------------------------------------------- |
| `parse_i32`  | `fn parse_i32(s: str) -> i32 ! str`   | Parse string to `i32` with error on invalid input. |
| `parse_i64`  | `fn parse_i64(s: str) -> i64 ! str`   | Parse string to `i64`.                             |
| `parse_f64`  | `fn parse_f64(s: str) -> f64 ! str`   | Parse string to `f64`.                             |
| `parse_bool` | `fn parse_bool(s: str) -> bool ! str` | Parse `"true"`/`"false"`.                          |

> **Note:** These could alternatively be `str` extension methods (see §2.1).
> Recommend intrinsic implementation because the runtime already has `rsg_str_from_*`
> and the reverse direction benefits from the same type-dispatch mechanism as `print`.

#### Memory & Unsafe

| Function | Signature                          | Description                                                                                  |
| -------- | ---------------------------------- | -------------------------------------------------------------------------------------------- |
| `copy`   | `fn copy<T: Clone>(value: T) -> T` | Explicit deep copy (identity for value types; clone for pointer types implementing `Clone`). |

---

## 2. Extension Methods for Primitive Types

Declared in `std/builtin.rsg` or relevant `std/*.rsg` modules. Methods on primitives
use the existing `ext Type { ... }` mechanism and are lowered as mangled function calls.

### 2.1 `str` Extensions

Currently only `.len()` exists.

| Method        | Signature                                   | Description                        |
| ------------- | ------------------------------------------- | ---------------------------------- |
| `len`         | `fn len(s) -> i32`                          | Exists                             |
| `is_empty`    | `fn is_empty(s) -> bool`                    | True when `len == 0`.              |
| `contains`    | `fn contains(s, needle: str) -> bool`       | Substring search.                  |
| `starts_with` | `fn starts_with(s, prefix: str) -> bool`    | Prefix test.                       |
| `ends_with`   | `fn ends_with(s, suffix: str) -> bool`      | Suffix test.                       |
| `index_of`    | `fn index_of(s, needle: str) -> ?i32`       | First occurrence index.            |
| `trim`        | `fn trim(s) -> str`                         | Strip leading/trailing whitespace. |
| `trim_start`  | `fn trim_start(s) -> str`                   | Strip leading whitespace.          |
| `trim_end`    | `fn trim_end(s) -> str`                     | Strip trailing whitespace.         |
| `to_upper`    | `fn to_upper(s) -> str`                     | ASCII uppercase.                   |
| `to_lower`    | `fn to_lower(s) -> str`                     | ASCII lowercase.                   |
| `split`       | `fn split(s, sep: str) -> []str`            | Split into slice of substrings.    |
| `replace`     | `fn replace(s, old: str, new: str) -> str`  | Replace all occurrences.           |
| `repeat`      | `fn repeat(s, n: i32) -> str`               | Repeat `n` times.                  |
| `substr`      | `fn substr(s, start: i32, end: i32) -> str` | Byte-offset substring.             |
| `char_at`     | `fn char_at(s, idx: i32) -> ?char`          | Byte-index access to char.         |
| `chars`       | `fn chars(s) -> []char`                     | Convert to character slice.        |
| `bytes`       | `fn bytes(s) -> []u8`                       | Raw byte slice.                    |
| `to_i32`      | `fn to_i32(s) -> i32 ! str`                 | Parse to i32.                      |
| `to_i64`      | `fn to_i64(s) -> i64 ! str`                 | Parse to i64.                      |
| `to_f64`      | `fn to_f64(s) -> f64 ! str`                 | Parse to f64.                      |

### 2.2 Integer Extensions (`i8`–`i128`, `u8`–`u128`, `isize`, `usize`)

Recommend implementing on `i32` first, then generalizing via generic ext or per-type ext blocks.

| Method   | Signed | Unsigned | Signature                                 | Description             |
| -------- | ------ | -------- | ----------------------------------------- | ----------------------- |
| `abs`    | ✔      | —        | `fn abs(n) -> Self`                       | Absolute value.         |
| `min`    | ✔      | ✔        | `fn min(n, other: Self) -> Self`          | Smaller of two values.  |
| `max`    | ✔      | ✔        | `fn max(n, other: Self) -> Self`          | Larger of two values.   |
| `clamp`  | ✔      | ✔        | `fn clamp(n, lo: Self, hi: Self) -> Self` | Clamp to `[lo, hi]`.    |
| `to_str` | ✔      | ✔        | `fn to_str(n) -> str`                     | String representation.  |
| `to_f64` | ✔      | ✔        | `fn to_f64(n) -> f64`                     | Widen to float.         |
| `pow`    | ✔      | ✔        | `fn pow(n, exp: u32) -> Self`             | Integer exponentiation. |

### 2.3 Float Extensions (`f32`, `f64`)

| Method      | Signature                                 | Description                              |
| ----------- | ----------------------------------------- | ---------------------------------------- |
| `abs`       | `fn abs(n) -> Self`                       | Absolute value.                          |
| `floor`     | `fn floor(n) -> Self`                     | Round toward negative infinity.          |
| `ceil`      | `fn ceil(n) -> Self`                      | Round toward positive infinity.          |
| `round`     | `fn round(n) -> Self`                     | Round to nearest integer.                |
| `trunc`     | `fn trunc(n) -> Self`                     | Truncate toward zero.                    |
| `sqrt`      | `fn sqrt(n) -> Self`                      | Square root.                             |
| `min`       | `fn min(n, other: Self) -> Self`          | Smaller of two values (NaN-propagating). |
| `max`       | `fn max(n, other: Self) -> Self`          | Larger of two values.                    |
| `clamp`     | `fn clamp(n, lo: Self, hi: Self) -> Self` | Clamp to range.                          |
| `is_nan`    | `fn is_nan(n) -> bool`                    | NaN test.                                |
| `is_inf`    | `fn is_inf(n) -> bool`                    | Infinity test.                           |
| `is_finite` | `fn is_finite(n) -> bool`                 | Finite test.                             |
| `to_i32`    | `fn to_i32(n) -> i32`                     | Truncate to integer.                     |
| `to_str`    | `fn to_str(n) -> str`                     | String representation.                   |
| `pow`       | `fn pow(n, exp: Self) -> Self`            | Exponentiation.                          |
| `ln`        | `fn ln(n) -> Self`                        | Natural logarithm.                       |
| `log2`      | `fn log2(n) -> Self`                      | Base-2 logarithm.                        |
| `log10`     | `fn log10(n) -> Self`                     | Base-10 logarithm.                       |
| `sin`       | `fn sin(n) -> Self`                       | Sine.                                    |
| `cos`       | `fn cos(n) -> Self`                       | Cosine.                                  |
| `tan`       | `fn tan(n) -> Self`                       | Tangent.                                 |

### 2.4 `bool` Extensions

| Method   | Signature             | Description           |
| -------- | --------------------- | --------------------- |
| `to_str` | `fn to_str(b) -> str` | `"true"` / `"false"`. |
| `to_i32` | `fn to_i32(b) -> i32` | `1` / `0`.            |

### 2.5 `char` Extensions

| Method            | Signature                       | Description              |
| ----------------- | ------------------------------- | ------------------------ |
| `is_alpha`        | `fn is_alpha(c) -> bool`        | Alphabetic (letter).     |
| `is_digit`        | `fn is_digit(c) -> bool`        | ASCII digit `0`–`9`.     |
| `is_alphanumeric` | `fn is_alphanumeric(c) -> bool` | Letter or digit.         |
| `is_whitespace`   | `fn is_whitespace(c) -> bool`   | Whitespace character.    |
| `is_upper`        | `fn is_upper(c) -> bool`        | Uppercase letter.        |
| `is_lower`        | `fn is_lower(c) -> bool`        | Lowercase letter.        |
| `to_upper`        | `fn to_upper(c) -> char`        | Uppercase conversion.    |
| `to_lower`        | `fn to_lower(c) -> char`        | Lowercase conversion.    |
| `to_str`          | `fn to_str(c) -> str`           | Single-character string. |
| `to_i32`          | `fn to_i32(c) -> i32`           | Unicode code point.      |

### 2.6 Slice Extensions (`[]T`)

Currently only `.len()` exists.

| Method      | Signature                                       | Description                                             |
| ----------- | ----------------------------------------------- | ------------------------------------------------------- |
| `len`       | `fn len(s) -> i32`                              | Exists.                                                 |
| `is_empty`  | `fn is_empty(s) -> bool`                        | True when `len == 0`.                                   |
| `first`     | `fn first(s) -> ?T`                             | First element or `None`.                                |
| `last`      | `fn last(s) -> ?T`                              | Last element or `None`.                                 |
| `contains`  | `fn contains(s, item: T) -> bool`               | Linear search (requires `T: Eq`).                       |
| `index_of`  | `fn index_of(s, item: T) -> ?i32`               | First occurrence index (requires `T: Eq`).              |
| `reverse`   | `fn reverse(s) -> []T`                          | New reversed slice.                                     |
| `map`       | `fn map<U>(s, f: Fn(T) -> U) -> []U`            | Transform elements.                                     |
| `filter`    | `fn filter(s, f: Fn(T) -> bool) -> []T`         | Select matching elements.                               |
| `reduce`    | `fn reduce(s, f: Fn(T, T) -> T) -> ?T`          | Fold from first element.                                |
| `fold`      | `fn fold<U>(s, init: U, f: Fn(U, T) -> U) -> U` | Fold with accumulator.                                  |
| `any`       | `fn any(s, f: Fn(T) -> bool) -> bool`           | True if any element matches.                            |
| `all`       | `fn all(s, f: Fn(T) -> bool) -> bool`           | True if all elements match.                             |
| `push`      | `fn push(mut *s, item: T)`                      | Append element (reallocate).                            |
| `pop`       | `fn pop(mut *s) -> ?T`                          | Remove and return last element.                         |
| `join`      | `fn join(s, sep: str) -> str`                   | Join `[]str` with separator (constrained to `T = str`). |
| `sort`      | `fn sort(mut *s)`                               | In-place sort (requires `T: Ord`).                      |
| `enumerate` | `fn enumerate(s) -> [](i32, T)`                 | Pair each element with its index.                       |
| `zip`       | `fn zip<U>(s, other: []U) -> [](T, U)`          | Pair elements from two slices.                          |
| `flat_map`  | `fn flat_map<U>(s, f: Fn(T) -> []U) -> []U`     | Map and flatten.                                        |
| `chunks`    | `fn chunks(s, size: i32) -> [][]T`              | Split into fixed-size chunks.                           |
| `windows`   | `fn windows(s, size: i32) -> [][]T`             | Sliding windows.                                        |
| `dedup`     | `fn dedup(s) -> []T`                            | Remove consecutive duplicates (requires `T: Eq`).       |
| `clone`     | `fn clone(s) -> []T`                            | Deep copy of slice.                                     |

### 2.7 Array Extensions (`[N]T`)

| Method     | Signature                             | Description                       |
| ---------- | ------------------------------------- | --------------------------------- |
| `len`      | `fn len(s) -> i32`                    | Exists (constant fold).           |
| `contains` | `fn contains(s, item: T) -> bool`     | Linear search (requires `T: Eq`). |
| `reverse`  | `fn reverse(s) -> [N]T`               | New reversed array.               |
| `map`      | `fn map<U>(s, f: Fn(T) -> U) -> [N]U` | Transform elements.               |

### 2.8 Option Extensions (`?T`)

| Method      | Signature                                 | Description                    |
| ----------- | ----------------------------------------- | ------------------------------ |
| `is_some`   | `fn is_some(o) -> bool`                   | True if `Some`.                |
| `is_none`   | `fn is_none(o) -> bool`                   | True if `None`.                |
| `unwrap`    | `fn unwrap(o) -> T`                       | Extract value or panic.        |
| `unwrap_or` | `fn unwrap_or(o, fallback: T) -> T`       | Extract or use default.        |
| `map`       | `fn map<U>(o, f: Fn(T) -> U) -> ?U`       | Transform inner value.         |
| `and_then`  | `fn and_then<U>(o, f: Fn(T) -> ?U) -> ?U` | Flat map.                      |
| `or_else`   | `fn or_else(o, f: Fn() -> ?T) -> ?T`      | Fallback producer.             |
| `ok_or`     | `fn ok_or<E>(o, err: E) -> T ! E`         | Convert to Result.             |
| `expect`    | `fn expect(o, msg: str) -> T`             | Extract or panic with message. |

### 2.9 Result Extensions (`T ! E`)

| Method       | Signature                                       | Description                          |
| ------------ | ----------------------------------------------- | ------------------------------------ |
| `is_ok`      | `fn is_ok(r) -> bool`                           | True if `Ok`.                        |
| `is_err`     | `fn is_err(r) -> bool`                          | True if `Err`.                       |
| `unwrap`     | `fn unwrap(r) -> T`                             | Extract `Ok` value or panic.         |
| `unwrap_err` | `fn unwrap_err(r) -> E`                         | Extract `Err` value or panic.        |
| `unwrap_or`  | `fn unwrap_or(r, fallback: T) -> T`             | Extract or use default.              |
| `map`        | `fn map<U>(r, f: Fn(T) -> U) -> U ! E`          | Transform `Ok` value.                |
| `map_err`    | `fn map_err<F>(r, f: Fn(E) -> F) -> T ! F`      | Transform `Err` value.               |
| `and_then`   | `fn and_then<U>(r, f: Fn(T) -> U ! E) -> U ! E` | Chain fallible operations.           |
| `or_else`    | `fn or_else<F>(r, f: Fn(E) -> T ! F) -> T ! F`  | Recover from error.                  |
| `ok`         | `fn ok(r) -> ?T`                                | Convert to Option, discarding error. |
| `expect`     | `fn expect(r, msg: str) -> T`                   | Extract or panic with message.       |

---

## 3. Standard Library Modules

### Module Layout

```plain
std/
├── builtin.rsg      # Exists — always injected (Option, Result, intrinsics)
├── prelude.rsg      # Exists — auto-imported convenience re-exports
├── io.rsg           # Console, file I/O, buffered readers/writers
├── fs.rsg           # File system operations
├── os.rsg           # OS interaction (env vars, args, exit)
├── math.rsg         # Mathematical constants and functions
├── str.rsg          # Extended string operations
├── conv.rsg         # Type conversions (Into, TryInto, Display, FromStr pacts)
├── fmt.rsg          # String formatting utilities
├── collections.rsg  # Map, Set, Queue, Stack
├── sort.rsg         # Sorting algorithms
├── rand.rsg         # Random number generation
├── time.rsg         # Time measurement and formatting
├── path.rsg         # File path manipulation
├── json.rsg         # JSON serialization/deserialization
├── testing.rsg      # Test assertion helpers
```

---

### 3.1 `std/io` — Input / Output

The current `std/io.rsg` is a placeholder. `print`/`println` live in `builtin.rsg` as
intrinsics. This module should provide higher-level I/O.

```rsg
// ── Console I/O ──

/// Read a line from stdin (without trailing newline).
pub fn read_line() -> str ! str

/// Read all of stdin as a string.
pub fn read_all() -> str ! str

/// Write raw bytes to stdout.
pub fn write(data: []u8) -> unit ! str

/// Write raw bytes to stderr.
pub fn eprint(msg: str)

/// Write to stderr with newline.
pub fn eprintln(msg: str)

// ── File I/O ──

pub struct File { /* opaque */ }

pub fn open(path: str) -> File ! str
pub fn create(path: str) -> File ! str

ext File {
    pub fn read_all(*f) -> str ! str
    pub fn read_bytes(*f) -> []u8 ! str
    pub fn write(mut *f, data: str) -> unit ! str
    pub fn write_bytes(mut *f, data: []u8) -> unit ! str
    pub fn close(mut *f)
}

// ── Buffered I/O ──

pub struct BufReader { /* opaque */ }

pub fn buf_reader(f: *File) -> BufReader

ext BufReader {
    pub fn next_line(mut *r) -> ?str ! str
    pub fn read_all(mut *r) -> str ! str
}
```

**Runtime mapping:**

- `read_line` → `rsg_io_read_line` (uses `fgets` + GC string allocation)
- `open` / `create` → `rsg_io_open` / `rsg_io_create` (wraps `fopen`)
- File methods → corresponding `fread` / `fwrite` / `fclose` wrappers

---

### 3.2 `std/fs` — File System

```rsg
/// Read an entire file to string.
pub fn read_file(path: str) -> str ! str

/// Write a string to file (creates or truncates).
pub fn write_file(path: str, content: str) -> unit ! str

/// Append a string to file.
pub fn append_file(path: str, content: str) -> unit ! str

/// Check if a path exists.
pub fn exists(path: str) -> bool

/// Remove a file.
pub fn remove(path: str) -> unit ! str

/// Rename/move a file.
pub fn rename(old: str, new: str) -> unit ! str

/// Create a directory (and parents).
pub fn mkdir(path: str) -> unit ! str

/// List directory entries.
pub fn read_dir(path: str) -> []str ! str
```

---

### 3.3 `std/os` — Operating System

```rsg
/// Command-line arguments (excluding program name).
pub fn args() -> []str

/// Get environment variable.
pub fn env(key: str) -> ?str

/// Set environment variable.
pub fn set_env(key: str, value: str)

/// Exit the process with a status code.
pub fn exit(code: i32) -> never
```

---

### 3.4 `std/math` — Mathematics

Wraps C `<math.h>` via `declare fn` with `link_name`.

```rsg
// ── Constants ──
pub var PI: f64 = 3.14159265358979323846
pub var E: f64 = 2.71828182845904523536
pub var TAU: f64 = 6.28318530717958647692
pub var INF: f64     // positive infinity
pub var NEG_INF: f64 // negative infinity
pub var NAN: f64     // quiet NaN

pub var MAX_I32: i32 = 2147483647
pub var MIN_I32: i32 = -2147483648
pub var MAX_I64: i64 = 9223372036854775807
pub var MIN_I64: i64 = -9223372036854775808
pub var MAX_F64: f64 = 1.7976931348623157e+308
pub var MIN_F64: f64 = 2.2250738585072014e-308 // smallest positive normal

// ── Functions ──
pub fn abs_i32(x: i32) -> i32
pub fn abs_f64(x: f64) -> f64
pub fn min_i32(a: i32, b: i32) -> i32
pub fn max_i32(a: i32, b: i32) -> i32
pub fn min_f64(a: f64, b: f64) -> f64
pub fn max_f64(a: f64, b: f64) -> f64
pub fn clamp_i32(x: i32, lo: i32, hi: i32) -> i32
pub fn clamp_f64(x: f64, lo: f64, hi: f64) -> f64

pub fn floor(x: f64) -> f64
pub fn ceil(x: f64) -> f64
pub fn round(x: f64) -> f64
pub fn trunc(x: f64) -> f64
pub fn sqrt(x: f64) -> f64
pub fn cbrt(x: f64) -> f64
pub fn pow(base: f64, exp: f64) -> f64

pub fn ln(x: f64) -> f64
pub fn log2(x: f64) -> f64
pub fn log10(x: f64) -> f64

pub fn sin(x: f64) -> f64
pub fn cos(x: f64) -> f64
pub fn tan(x: f64) -> f64
pub fn asin(x: f64) -> f64
pub fn acos(x: f64) -> f64
pub fn atan(x: f64) -> f64
pub fn atan2(y: f64, x: f64) -> f64

pub fn is_nan(x: f64) -> bool
pub fn is_inf(x: f64) -> bool
pub fn is_finite(x: f64) -> bool
```

---

### 3.5 `std/str` — Extended String Utilities

Houses heavier string operations beyond the core `ext str` methods. The `ext str`
methods in `builtin.rsg` (or a new `std/str_ext.rsg`) provide the method syntax;
this module provides free functions for algorithms that don't have a natural receiver.

```rsg
/// Join a slice of strings with a separator.
pub fn join(parts: []str, sep: str) -> str

/// Repeat a string N times.
pub fn repeat(s: str, n: i32) -> str

/// Format an integer in the given base (2, 8, 10, 16).
pub fn format_int(val: i64, base: i32) -> str

/// Left-pad to minimum width.
pub fn pad_left(s: str, width: i32, fill: char) -> str

/// Right-pad to minimum width.
pub fn pad_right(s: str, width: i32, fill: char) -> str
```

---

### 3.6 `std/conv` — Conversion Pacts

Define the standard conversion contracts that the ecosystem builds on.

```rsg
/// Infallible conversion (no data loss).
pub pact Into<T> {
    fn into(self) -> T
}

/// Fallible conversion.
pub pact TryInto<T> {
    type Error
    fn try_into(self) -> T ! Self::Error
}

/// Convert from another type.
pub pact From<T> {
    fn from(value: T) -> Self
}

/// Human-readable string representation.
pub pact Display {
    fn to_string(*self) -> str
}

/// Parse from string.
pub pact FromStr {
    type Error
    fn from_str(s: str) -> Self ! Self::Error
}

/// Debug representation (for development/logging).
pub pact Debug {
    fn debug_str(*self) -> str
}

/// Equality comparison.
pub pact Eq {
    fn eq(*self, other: *Self) -> bool
}

/// Ordering comparison.
pub pact Ord {
    Eq
    fn cmp(*self, other: *Self) -> i32  // -1, 0, 1
}

/// Hashable type.
pub pact Hash {
    fn hash(*self) -> u64
}

/// Type with a default/zero value.
pub pact Default {
    fn default() -> Self
}

/// Cloneable type (explicit deep copy).
pub pact Clone {
    fn clone(*self) -> Self
}
```

---

### 3.7 `std/collections` — Data Structures

```rsg
// ── HashMap ──

pub struct Map<K: Hash + Eq, V> { /* opaque */ }

ext<K: Hash + Eq, V> Map<K, V> {
    pub fn new() -> Self
    pub fn insert(mut *self, key: K, value: V) -> ?V   // returns old value
    pub fn get(*self, key: K) -> ?V
    pub fn remove(mut *self, key: K) -> ?V
    pub fn contains_key(*self, key: K) -> bool
    pub fn len(*self) -> i32
    pub fn is_empty(*self) -> bool
    pub fn keys(*self) -> []K
    pub fn values(*self) -> []V
    pub fn entries(*self) -> [](K, V)
    pub fn clear(mut *self)
}

// ── HashSet ──

pub struct Set<T: Hash + Eq> { /* opaque */ }

ext<T: Hash + Eq> Set<T> {
    pub fn new() -> Self
    pub fn insert(mut *self, item: T) -> bool  // true if new
    pub fn remove(mut *self, item: T) -> bool
    pub fn contains(*self, item: T) -> bool
    pub fn len(*self) -> i32
    pub fn is_empty(*self) -> bool
    pub fn items(*self) -> []T
    pub fn union(*self, other: *Self) -> Self
    pub fn intersection(*self, other: *Self) -> Self
    pub fn difference(*self, other: *Self) -> Self
}

// ── Queue (FIFO) ──

pub struct Queue<T> { /* opaque */ }

ext<T> Queue<T> {
    pub fn new() -> Self
    pub fn enqueue(mut *self, item: T)
    pub fn dequeue(mut *self) -> ?T
    pub fn peek(*self) -> ?T
    pub fn len(*self) -> i32
    pub fn is_empty(*self) -> bool
}

// ── Stack (LIFO) ──

pub struct Stack<T> { /* opaque */ }

ext<T> Stack<T> {
    pub fn new() -> Self
    pub fn push(mut *self, item: T)
    pub fn pop(mut *self) -> ?T
    pub fn peek(*self) -> ?T
    pub fn len(*self) -> i32
    pub fn is_empty(*self) -> bool
}
```

---

### 3.8 `std/sort` — Sorting

```rsg
/// In-place sort a mutable slice (stable, requires T: Ord).
pub fn sort<T: Ord>(mut s: *[]T)

/// Sort with a custom comparator. cmp returns -1/0/1.
pub fn sort_by<T>(mut s: *[]T, cmp: Fn(*T, *T) -> i32)

/// Check if a slice is sorted.
pub fn is_sorted<T: Ord>(s: []T) -> bool

/// Binary search on a sorted slice. Returns index or None.
pub fn binary_search<T: Ord>(s: []T, target: T) -> ?i32
```

---

### 3.9 `std/rand` — Random Numbers

```rsg
/// Seed the global RNG.
pub fn seed(s: u64)

/// Random i32 in [lo, hi].
pub fn rand_i32(lo: i32, hi: i32) -> i32

/// Random f64 in [0.0, 1.0).
pub fn rand_f64() -> f64

/// Shuffle a mutable slice in-place.
pub fn shuffle<T>(mut s: *[]T)
```

---

### 3.10 `std/time` — Time

```rsg
/// Monotonic nanosecond timestamp (for measuring durations).
pub fn now_ns() -> i64

/// Wall-clock seconds since Unix epoch.
pub fn unix_secs() -> i64

/// Sleep for the given number of milliseconds.
pub fn sleep_ms(ms: i64)
```

---

### 3.11 `std/path` — Path Manipulation

Pure string manipulation (no I/O).

```rsg
/// Join path segments with the platform separator.
pub fn join(parts: ..str) -> str

/// Extract the file name from a path.
pub fn file_name(path: str) -> str

/// Extract the directory portion.
pub fn dir(path: str) -> str

/// Extract the file extension (without dot), or None.
pub fn ext(path: str) -> ?str

/// Check if a path is absolute.
pub fn is_absolute(path: str) -> bool
```

---

### 3.12 `std/testing` — Test Helpers

Utilities for the planned `v1.1` test framework.

```rsg
/// Assert equality with descriptive failure message.
pub fn assert_eq<T: Eq + Display>(actual: T, expected: T)

/// Assert inequality.
pub fn assert_ne<T: Eq + Display>(actual: T, expected: T)

/// Assert that a Result is Ok.
pub fn assert_ok<T, E: Display>(result: T ! E)

/// Assert that a Result is Err.
pub fn assert_err<T, E>(result: T ! E)

/// Assert that an Option is Some.
pub fn assert_some<T>(opt: ?T)

/// Assert that an Option is None.
pub fn assert_none<T>(opt: ?T)
```

---

### 3.13 `std/fmt` — Formatting

```rsg
/// Sprintf-style formatting with interpolation.
pub fn format(template: str, args: ..str) -> str

/// Format a float with fixed decimal places.
pub fn fixed(value: f64, decimals: i32) -> str

/// Format an integer with thousand separators.
pub fn thousands(value: i64, sep: char) -> str

/// Hex representation of an integer.
pub fn hex(value: i64) -> str

/// Binary representation of an integer.
pub fn bin(value: i64) -> str

/// Octal representation of an integer.
pub fn oct(value: i64) -> str
```

---

## 4. Prelude Updates

The prelude (`std/prelude.rsg`) should auto-import the most commonly used items from
the standard library, so users don't need explicit `use` for everyday operations.

### Currently in Prelude

- `Option<T>`, `Result<T, E>` (via `builtin.rsg`)
- `print`, `println`, `assert`, `len`, `panic`, `catch_panic` (via `builtin.rsg`)

### Proposed Prelude Additions

```rsg
// Re-export conversion pacts
pub use std::conv::{Display, Debug, Clone, Default, Eq, Ord, Hash, Into, From}

// Re-export common I/O
pub use std::io::{read_line, eprint, eprintln}

// Re-export Option/Result extension methods
// (available automatically via ext blocks in builtin.rsg)
```

---

## 5. Implementation Priority

Ordered by user impact and dependency chain.

### Phase 1 — Foundation (v0.9.13a)

High impact, low dependency. Enables idiomatic Resurg programs.

1. **`std/conv`** — `Display`, `Eq`, `Ord`, `Clone`, `Default`, `Hash`, `Into`, `From`, `FromStr`
2. **`str` extensions** — `is_empty`, `contains`, `starts_with`, `ends_with`, `trim`, `split`, `to_upper`, `to_lower`, `replace`, `substr`
3. **`char` extensions** — `is_alpha`, `is_digit`, `is_whitespace`, `to_upper`, `to_lower`
4. **Integer/float `.to_str()`** — enables `Display` conformance
5. **`?T` / `T ! E` extensions** — `unwrap`, `unwrap_or`, `map`, `is_some`, `is_ok`

### Phase 2 — Collections & Algorithms (v0.9.13b)

Requires Phase 1 (`Hash`, `Eq`, `Ord`).

1. **`[]T` extensions** — `contains`, `first`, `last`, `map`, `filter`, `fold`, `any`, `all`, `push`, `pop`, `reverse`, `sort`
2. **`std/collections`** — `Map`, `Set`
3. **`std/sort`** — `sort`, `sort_by`, `binary_search`
4. **`std/math`** — constants, trig, pow, floor/ceil/round

### Phase 3 — I/O & OS (v0.9.13c)

Requires Phase 1 (`Display` for errors).

1. **`std/io`** — `read_line`, `eprint`, `eprintln`, `File` type, `BufReader`
2. **`std/fs`** — `read_file`, `write_file`, `exists`, `mkdir`, `read_dir`
3. **`std/os`** — `args`, `env`, `exit`

### Phase 4 — Ergonomics (v0.9.13d)

1. **`std/fmt`** — `fixed`, `hex`, `bin`
2. **`std/time`** — `now_ns`, `sleep_ms`
3. **`std/rand`** — `rand_i32`, `rand_f64`, `shuffle`
4. **`std/path`** — `join`, `file_name`, `ext`
5. **`std/testing`** — `assert_eq`, `assert_ne`, `assert_ok`, `assert_some`
6. **Builtin `sizeof` / `type_name`** — compiler intrinsics

---

## 6. Runtime Implementation Notes

### Adding a Non-Intrinsic Stdlib Function

For functions that don't need special compiler support (no type dispatch, no source injection):

1. Add `pub declare fn name(...) = "c_link_name"` in the appropriate `std/*.rsg` file
2. Implement `c_link_name()` in `runtime/rsg_<module>.c`
3. Declare in `runtime/rsg_<module>.h`
4. Include in `runtime/rsg_runtime.h` umbrella
5. Add to CMake sources

### Adding an Extension Method on a Primitive

Two implementation strategies:

**A. Pure Resurg (preferred when no C dependency):**

```rsg
// std/builtin.rsg or std/str_ext.rsg
ext str {
    pub fn is_empty(s) -> bool { s.len() == 0 }
}
```

**B. Declare + C runtime (when C library access needed):**

```rsg
// std/builtin.rsg
ext str {
    pub declare fn contains(s, needle: str) -> bool = "rsg_str_contains"
}
```

```c
// runtime/rsg_str.c
bool rsg_str_contains(RsgStr haystack, RsgStr needle) {
    if (needle.len == 0) return true;
    if (needle.len > haystack.len) return false;
    return memmem(haystack.data, haystack.len, needle.data, needle.len) != NULL;
}
```

### GC Considerations

All `str` and `[]T` values returned from runtime functions must be GC-allocated
(via `rsg_heap_alloc`) so the conservative GC can track them. The existing
`rsg_str_new` and `rsg_slice_new` already handle this.

---

## 7. Design Decisions & Rationale

| Decision                                             | Rationale                                                                                     |
| ---------------------------------------------------- | --------------------------------------------------------------------------------------------- |
| Extension methods over free functions for primitives | Discoverable via `.` completion; natural to the language idiom                                |
| `std/conv` pacts early                               | Enables generic algorithms (`sort<T: Ord>`, `Map<K: Hash + Eq>`)                              |
| Separate `std/math` module vs float extensions       | Extensions for common ops (`.abs()`, `.floor()`); module for constants and advanced functions |
| `str` parsing as extensions (`.to_i32()`)            | Matches `i32.to_str()` symmetry; no need for standalone `parse_*` intrinsics                  |
| `Option`/`Result` methods in `builtin.rsg`           | Always available; most commonly used utility methods after `.len()`                           |
| `Map`/`Set` before `Queue`/`Stack`                   | Higher demand; Queue/Stack trivially built on slices                                          |
| `declare fn ... = "link_name"` for most stdlib       | Avoids adding intrinsics; link_name mechanism already works                                   |
