# Resurg Programming Language

Statically typed, compiled. ADTs, pattern matching, value-based errors. **Extension:** `.rsg`

## Compiler

Written in C17. Compiles `.rsg` source to C17. Future back-ends: C++20, Go.

---

## 1. Design Principles

- No null — `?T` option types
- No exceptions — `T ! E` result types, postfix `!` propagation
- Composition over inheritance — struct embedding + pacts (interfaces)
- Private by default — `pub` to export
- No implicit conversions

---

## 2. Type System

### Primitives

`bool`, `char`, `i8`–`i128`, `isize`, `u8`–`u128`, `usize`, `f32`, `f64`, `str` (UTF-8), `unit` (zero-size type).

**Char semantics:** `char` is a Unicode scalar value. Supports equality (`==`, `!=`) and ordering (`<`, `>`, `<=`, `>=`). Escape sequences: `\n`, `\t`, `\\`, `\'`, `\0`, `\u{XXXX}`.

**Float semantics:** Negative zero equals positive zero (`-0.0 == 0.0`). Scientific notation supported: `1.5e2`, `2.0e-1`.

**Numeric literals:** Underscore separators allowed for readability: `1_000_000`, `1_000.5`.

**String comparison:** `str` supports `==` and `!=` for equality testing.

### Compound Types

| Kind     | Syntax       | Example                    | Notes                            |
| -------- | ------------ | -------------------------- | -------------------------------- |
| Array    | `[N]T`       | `[5]i32`                   | Fixed-size                       |
| Slice    | `[]T`        | `[]i32`                    | Fat pointer (data + length)      |
| Pointer  | `*T`         | `*i32`                     | No arithmetic                    |
| Tuple    | `(A, B, …)`  | `(i32, str)`               | Heterogeneous, ≥ 2 elements      |
| Option   | `?T`         | `Some(value)` / `None`     | Compiler-enforced null safety    |
| Result   | `T ! E`      | `Ok(value)` / `Err(error)` | Compiler-enforced error handling |
| Function | `fn(…) -> R` | `fn(i32) -> bool`          | First-class, supports closures   |

**Tuples** support `==` and `!=` for equality comparison. Nested tuple fields are accessed by chaining indices: `t.0.1`.

### Type Aliases

```rsg
type ID = u64
type Handler = fn(Request) -> Response
```

### Declarations & Inference

```rsg
count := 0               // inferred i32
var name: str = "Alice"  // explicit type
x := i32(10)             // typed literal — equivalent to var x: i32 = 10
y := f32(3.14)           // typed literal — equivalent to var y: f32 = 3.14
var num: ?i32             // defaults to None

{name, age} := user           // destructure struct
(first, second) := get_pair() // destructure tuple
```

No return annotation implies `unit`. Variable redefinition in the same scope is forbidden — no shadowing.

---

## 3. Safety Model

### Option (`?T`)

Sugar for `enum Option<T> { None, Some(T) }`.

```rsg
fn find_user(id: u64) -> ?str {
    if id == 1 { Some("Alice") } else { None }
} 

user?.address?.city           // optional chaining
```

### Error Handling (`T ! E`)

Sugar for `enum Result<T, E> { Ok(T), Err(E) }`. Postfix `!` propagates.

```rsg
fn divide(a: f32, b: f32) -> f32 ! str {
    if b == 0.0 { Err("division by zero") }
    else { Ok(a / b) }
}

fn calc() -> f32 ! str {
    x := divide(10.0, 2.0)!         // propagates Err to caller
    y := divide(x, 5.0)!
    y
}
```

### Defer

LIFO cleanup on scope exit.

```rsg
f := open(path)!
defer f.close()
```

### Assert

Runtime assertion. Panics immediately on failure.

```rsg
assert x > 0
assert x > 0, "x must be positive"
```

### Panic & Recover

`panic` immediately aborts the current execution path with a message. `recover` may only be called inside a `defer` block — it returns `Some(message)` if a panic is active and catches it, or `None` otherwise.

Code after a `panic` call is unreachable. If no `recover` catches the panic, the program terminates with a diagnostic.

```rsg
fn explode() {
    panic("something went terribly wrong")
}

fn safe_call() -> ?str {
    var result: ?str = None
    defer {
        if Some(msg) := recover() {
            result = Some(msg)
        }
    }
    explode()
    result
}

msg := safe_call()   // Some("something went terribly wrong")
```

### Memory Model

**Tracing GC.** Structs are value types (copied on assignment). `&` heap-allocates and returns `*T`. No pointer arithmetic. Auto-deref on field access (`p.field` works on `*T`).

**Parameter passing:** `*T` parameter receives a pointer; caller must pass `&` explicitly for value types.

```rsg
struct Node {
    value: i32
    children: []*Node
    parent: ?*Node
}

fn build_tree() -> *Node {
    root := &Node { value = 0 }
    child := &Node { value = 1, parent = Some(root) }
    root.children = [child]
    root
}
```

**Arrays:** `[N]T` is a fixed-size value type. Copied on assignment.

```rsg
// declaration forms
arr: [5]i32 = [1, 2, 3, 4, 5]
arr2 := [5]i32[1, 2, 3, 4, 5]

// arrays are values — assignment copies
a := [3]i32[10, 20, 30]
b := a                   // b is an independent copy
b[0] = 99               // a[0] is still 10

// equality comparison
[3]i32[1, 2, 3] == [3]i32[1, 2, 3]  // true

// when the left-hand side has a type annotation, the literal can omit the type prefix
var arr: [3]i32 = [1, 2, 3]
```

**Slices:** `[]T` is a GC-backed fat pointer (data + length). Array-to-slice copies into GC storage. Sub-slicing shares backing storage.

```rsg
arr := [5]i32[1, 2, 3, 4, 5]

// array → slice: copies data into GC storage
s: []i32 = arr[..] // equals s := []i32[1, 2, 3, 4, 5]

// sub-slicing: shares backing storage (no copy)
t := s[1..4]             // [2, 3, 4]
u := s[2..]              // [3, 4, 5]
v := s[..3]              // [1, 2, 3]

// slices are reference types — aliases see mutations
x := s[1..4]
x[0] = 99               // s[1] is now 99 too

// slice of slices
matrix: [][]*i32 = []
```

### Mutability (`mut`)

`mut` only applies to pointers. Value types are copied — callee owns the copy. Pointer params are read-only by default; `mut` required on both declaration and call site.

```rsg
fn set_name(mut user: *User, name: str) {
    user.name = name
}
set_name(mut &user, "Bob")

// value param — callee gets a copy, can mutate freely
fn shift(p: Point, dx: f64) -> Point {
    p.x += dx
    p
}
```

### Immutable (`immut`)

Marks a binding, field as permanently immutable. On pointer bindings: prevents mutation, variable-to-variable assignment, and passing as `mut`. Function calls are still allowed.

```rsg
immut p := &Point { x = 1.0, y = 2.0 }
// p.x = 3.0        // ERROR: mutation
// q := p            // ERROR: pointer assignment
do_something(p)      // OK: function call

// value types: copy is independent
immut v := Point { x = 1.0, y = 2.0 }
w := v               // OK: value copy
```

**Immut fields and structs:**

```rsg
struct Service {
    immut db: *Database     // cannot assign to another variable
    name: str
}
```

**Immut return values:**

```rsg
fn create_logger() -> immut *Logger {
    &Logger { level = "info" }
}
```

---

## 4. Data Modeling

### Structs & Embedding

Value types. `*` is the receiver (self). Embedding promotes fields/methods; override by redefining.

```rsg
struct Point {
    x: f64 = 0.0
    y: f64 = 0.0
    fn set_position(mut *point, target: Point) {
        point.x = target.x
        point.y = target.y
    }
}

struct Point3D {
    Point                  // embedding
    z: f64 = 0.0
    fn set_position(mut *p, target: Point3D) {
        p.Point.set_position(x = target.x, y = target.y)
        p.z = target.z
    }
}
```

Pointer semantics with `&`:

```rsg
a := &Connection { host = "localhost", port = 8080 }
b := a          // pointer copy — same instance
```

### Enums (ADTs)

```rsg
enum Msg {
    Quit,
    Move { x: i32, y: i32 },
    Write(str),
}

enum Status { Active = 1, Inactive = 0, Pending = 2 }
```

### Pattern Matching

`match` is exhaustive — the compiler rejects non-total matches, preventing unhandled cases at compile time.

```rsg
match msg {
    Quit => println("quit"),
    Move { x, y } => println("{x}, {y}"),
    Write(text) => println(text),
}

// wildcard for catch-all
match msg {
    Move { x, y } => println("{x}, {y}"),
    _ => println("not move"),
}

// match guard: `if` condition after pattern
match msg {
    Move { x, y } if x > 0 && y > 0 => println("positive move"),
    Move { x, y } => println("other move: {x}, {y}"),
    _ => println("not move"),
}
```

Supported pattern kinds:

| Pattern          | Syntax                   |
| ---------------- | ------------------------ |
| Literal          | `200`, `"hello"`, `true` |
| Range            | `500..599` `500..=599`   |
| Wildcard         | `_`                      |
| Binding          | `name`                   |
| Variant (unit)   | `Quit`                   |
| Variant (tuple)  | `Write(text)`            |
| Variant (fields) | `Move { x, y }`          |
| Option           | `Some(v)` / `None`       |
| Result           | `Ok(v)` / `Err(e)`       |
| Guard            | `pattern if cond`        |

---

## 5. Abstraction & Extensibility

### Pacts (Interfaces)

Can require fields and methods, provide defaults. Explicit conformance via `struct Foo: Pact1 + Pact2`.

```rsg
pact Animal {
    name: str
    fn set_name(mut *animal, name: str) -> unit {
        animal.name = name
    }
}

pact Printable {
    fn to_string() -> str
}

struct Dog: Animal + Printable {
    fn to_string(*dog) = "Dog({dog.name})"
}
```

### Generics

Pact-bounded. Constraint aliases supported.

```rsg
fn max<T: Ord>(a: T, b: T) -> T {
    if a > b { a } else { b }
}

pact A { Ord; Display }       // or: pact A = Ord + Display

pact B<T> { Into<T>; Clone }

fn complex_merge<T: Ord + Display, U: B<T>>(a: []T, b: []U) -> []T { ... }
```

### Extension Methods

```rsg
ext User {
    fn is_adult(*s) = s.age >= 18
}

ext str {
    fn last_char(*s) -> char { s[s.len() - 1] }
}
```

---

## 6. Modules

Each `.rsg` file = module. Private by default.

```rsg
module math

use std/io
use std/io { read, write }
use std/io { read as rd }

pub fn add(a: i32, b: i32) = a + b
```

---

## 7. Functions & Closures

Expression or block bodies. Named args at call site. No nested functions; closures are lambda-only.

```rsg
fn add(a: i32, b: i32) = a + b

fn sum(values: []i32) -> i32 {
    total := 0
    for values |v| total += v
    total
}

set_name(mut user = &myUser, name = "Bob")
```

Closures:

```rsg
list.map(|x| x * 2).filter(|x| x > 10)
list.map(|x| x + offset)     // captures offset
```

### Pipe Operator

`|>` pipes the left-hand value as the first argument to the right-hand function.

```rsg
// equivalent: println(to_upper(trim(input)))
input |> trim |> to_upper |> println

// with additional arguments
values |> filter(|x| x > 0) |> map(|x| x * 2) |> sum

// works with closures
data |> |x| x + 1 |> println
```

---

## 8. Syntax Reference

### Keywords

```
assert break  continue  defer  else   enum   false  fn
for    if     loop      match  module mut    pact   pub
immut  return struct    true   type   use    var    while
```

**Reserved (future):**

```
async  await  comptime  macro  spawn
```

Conventions: `snake_case` identifiers, `PascalCase` types, `SCREAMING_CASE` constants. Comments: `//`.

### Operators (Resurg-specific)

| Op            | Meaning                     |
| ------------- | --------------------------- |
| `:=`          | Variable declaration        |
| `!` (postfix) | Error propagation           |
| `&`           | Address-of / create pointer |
| `*T` / `?T`   | Pointer / option type       |
| `?.`          | Optional chaining           |
| `..` `..=`    | Range (excl / incl)         |
| `\|>`         | Pipe (first-arg forwarding) |
| `\| \|`       | Closure / for-loop binding  |

### Precedence (high → low)

| Level | Operators                        |
| ----- | -------------------------------- |
| 1     | `.` `?.` `[]` `()`               |
| 2     | `!` (postfix)                    |
| 3     | `-` `!` `&` `*` (prefix)         |
| 4–5   | `*` `/` `%` then `+` `-`         |
| 6     | `..` `..=`                       |
| 7–8   | `<` `>` `<=` `>=` then `==` `!=` |
| 9–10  | `&&` then `\|\|`                 |
| 11    | `\|>`                            |
| 12    | `=` `:=` `+=` `-=` `*=` `/=`     |

**Integer division** truncates toward zero: `7 / 2 == 3`, `-7 / 2 == -3`.
**Modulo** follows truncation semantics (sign matches the dividend).

### Control Flow

`if`, `match`, `while`, and blocks are expressions. When used as a statement (not assigned), `if` may omit the `else` branch.

```rsg
result := if x > 10 { x } else { x + 1 }

// statement form — no else needed
if condition { do_something() }

while condition { do_work() }
loop { if done() { break } }
loop { if skip() { continue } }   // continue supported in loop
for values |v| println(v)
for values |v, i| ...
for 0..10 |i| ...
for start..end |i| ...             // variable bounds
for 0..n * 2 |i| ...              // computed bounds
```

An empty range (`N..N`) produces zero iterations.

### Pattern Binding (`if` / `while`)

Refutable patterns in `if` and `while` via `:=`. Binds on match; skips (or exits loop) on mismatch.

```rsg
// if-let: execute block when pattern matches
if Some(user) := find_user(id) {
    println("found: {user.name}")
}

if Some(user) := find_user(id) {
    println("found: {user.name}")
} else {
    println("not found")
}

// while-let: loop while pattern matches
while Some(line) := reader.next_line()! {
    process(line)
}

// works with any refutable pattern
if Ok(value) := parse_integer(input) {
    println("parsed: {value}")
}
```

### Literals

| Kind    | Examples                     | Default |
| ------- | ---------------------------- | ------- |
| Integer | `42`, `1_000_000`            | `i32`   |
| Float   | `3.14`, `2.5e10`             | `f64`   |
| Char    | `'a'`, `'\n'`, `'\u{1F600}'` |         |
| String  | `"hello"`, `"val: {x}"`      |         |
| Array   | `[1, 2, 3]`                  |         |
| Tuple   | `(1, "two", 3.0)`            | ≥ 2     |

String interpolation: `"Hello, {name}!"`, `"result: {a + b}"`