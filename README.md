# Resurg Programming Language

Statically typed, compiled. ADTs, pattern matching, value-based errors. **Extension:** `.rsg`

---

## 1. Design Principles

- No null — `?T` option types
- No exceptions — `T ! E` result types, postfix `!` propagation
- Composition over inheritance — struct embedding + pacts
- Private by default — `pub` to export
- No implicit conversions

---

## 2. Type System

### Primitives

`bool`, `char`, `i8`–`i128`, `isize`, `u8`–`u128`, `usize`, `f32`, `f64`, `str` (UTF-8), `unit`, `never`.

- `char`: Unicode scalar value. Supports `==`, `!=`, `<`, `>`, `<=`, `>=`. Escapes: `\n`, `\t`, `\\`, `\'`, `\0`, `\u{XXXX}`.
- `f32`/`f64`: `-0.0 == 0.0`. Scientific: `1.5e2`, `2.0e-1`.
- Numeric literals allow `_` separators: `1_000_000`, `1_000.5`.
- `str`: supports `==` and `!=`.

### Compound Types

| Kind     | Syntax       | Example                    | Notes                                         |
| -------- | ------------ | -------------------------- | --------------------------------------------- |
| Array    | `[N]T`       | `[5]i32`                   | Fixed-size value type                         |
| Slice    | `[]T`        | `[]i32`                    | Fat pointer (data + length)                   |
| Pointer  | `*T`         | `*i32`                     | No arithmetic                                 |
| Tuple    | `(A, B, …)`  | `(i32, str)`               | ≥ 2 elements, `==`/`!=`, chain access `t.0.1` |
| Option   | `?T`         | `Some(value)` / `None`     | Compiler-enforced null safety                 |
| Result   | `T ! E`      | `Ok(value)` / `Err(error)` | Compiler-enforced error handling              |
| Function | `fn(…) -> R` | `fn(i32) -> bool`          | First-class, supports closures                |

### Type Aliases

```rsg
type ID = u64
type Handler = fn(Request) -> Response
```

### `unit` & `never`

`unit`: The singleton type with exactly one value, **equivalent to the `()`**. It is the implicit return type when `->` is omitted.

`never`: type of computations that never produce a value.

```rsg
fn short() {}                        // returns unit (implicit)
fn explicit_unit() -> unit {}        // same as above
fn returns_zero_tuple() -> () {}    // also equivalent — () and unit are the same type

x: unit = ()                         // OK: zero tuple has type unit
y: () = unit                         // OK: unit has type ()
unit == ()                           // true
() == unit                           // true

match x {
    () => println("got unit/zero tuple"),
    _  => println!("B"), // ERROR: unreachable pattern
}

x := { return 123 }                  // x: never
```

---

## 3. Declarations, Bindings & Scope

### Declarations & Inference

```rsg
count := 0               // inferred i32
var name: str = "Alice"  // explicit type
x := i32(10)             // typed literal — equivalent to var x: i32 = 10
y := f32(3.14)           // typed literal — equivalent to var y: f32 = 3.14
var num: ?i32            // defaults to None

{name, age} := user           // destructure struct
{name: n, age: a} := user     // destructure struct with aliases
(first, second) := get_pair() // destructure tuple
{name} := user                // partial struct destructure
(_, second) := pair           // ignore element with _
(first, ..) := triple         // ignore trailing elements with ..
(.., last) := triple          // ignore leading elements with ..
(first, .., last) := quad     // ignore middle elements with ..
```

### Declare First

Variables declared without initialization must be assigned before use. Type is fixed at declaration.

```rsg
var a_binding: str
if cond { a_binding = "yes" } else { a_binding = "no" }

var another_binding: i32
another_binding // Error! Use of uninitialized binding
```

### Scope & Shadowing

Blocks are expressions. Shadowing allowed (lint-restrictable).

```rsg
fn main() {
    shadowed_binding := 1
    { shadowed_binding := "abc" } // shadows outer
    shadowed_binding := true      // shadows previous

    y := {
        x_squared := 5 * 5
        x_squared + 5 // assigned to y
    }
    empty := {}
}
```

---

## 4. Memory Model & Safety

### Memory Model

No pointer arithmetic. Auto-deref on field access (`p.field` works on `*T`).

**Struct:** Structs are value types (copied on assignment). `&` heap-allocates and returns `*T`.

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

**Parameter passing:** `*T` parameter receives a pointer; caller must pass `&` explicitly for value types.

| Category          | Types                                                          | Semantics |
| ----------------- | -------------------------------------------------------------- | --------- |
| **Value types**   | Primitives, `str`, `[N]T`, tuples, structs, enums, `?T`, `T!E` | Copied    |
| **Pointer types** | `*T` (pointer via `&`), `fn`                                   | Shared    |

Value types are **automatically copied** on assignment/passing — callee gets an independent copy.

```rsg
struct Point { x: f64; y: f64 }

fn shift(p: Point, dx: f64) -> Point {  // p is a copy
    p.x += dx
    p
}
origin := Point { x = 0.0, y = 0.0 }
moved := shift(origin, 5.0)  // origin unchanged

// To mutate the original, pass a pointer:
fn shift_mut(mut p: *Point, dx: f64) { p.x += dx }
shift_mut(&origin, 5.0)
```

**Arrays:** `[N]T` — fixed-size value type, copied on assignment.

```rsg
arr := [5]i32{1, 2, 3, 4, 5}
b := arr; b[0] = 99          // arr[0] still 1
[3]i32{1, 2, 3} == [3]i32{1, 2, 3}  // true
var arr2: [3]i32 = [1, 2, 3]         // type prefix omitted on right
```

**Slices:** `[]T` — GC-backed fat pointer. `arr[..]` **copies** array data into GC storage. Sub-slicing **shares** backing storage.

```rsg
// Essentially a slice, but with private fields to enforce abstraction as an opaque type.
struct CustomSlice {
    data: *T
    len: usize
}
```

```rsg
arr := [5]i32{1, 2, 3, 4, 5}
s: []i32 = arr[..]            // independent copy
s[0] = 99                     // arr[0] still 1

t := s[1..4]                  // [2, 3, 4] — shares storage with s
t[0] = 99                     // s[1] now 99 too
u := s[2..]; v := s[..3]      // sub-slicing
```

**Addressability:** Variables are addressable (`&var` → `*T`). Rvalues (literals, temporaries) are not — bind to a variable first. Exception: `&Struct` and `&[]slice` are heap-allocation syntax, not address-of.

```rsg
x := 42
px := &x                    // OK: *i32
// px := &42                // ERROR: rvalue

root := &Node { value = 0 } // heap allocation → *Node
process(&Point { x = 1.0, y = 2.0 })
slice_header := &[]i32{1, 2, 3}
```

**Pointer types:**

| Type   | Meaning                 | Typical Use                          |
| ------ | ----------------------- | ------------------------------------ |
| `*T`   | Pointer to `T`          | Heap allocation, optional references |
| `*[]T` | Pointer to slice header | Reassign slice (len/data) in-place   |
| `**T`  | Pointer to pointer      | Reassign pointer target in-place     |
| `[]*T` | Slice of pointers       | Collection of heap-allocated items   |

```rsg
fn extend(mut s: *[]i32) {
    *s = (*s)[..] + [99]   // reassign slice header
}

fn allocate(p: **Node) {
    *p = &Node{ value = 42 }
}
```

### Mutability (`mut`)

`mut` applies only to pointers. Value types are copied — callee owns the copy. Pointer params are read-only by default; `mut` required on both declaration and call site.

```rsg
fn set_name(mut user: *User, name: str) { user.name = name }
set_name(mut &user, "Bob")
```

### Immutable (`immut`)

Permanently immutable binding/field. On pointers: prevents mutation, pointer reassignment, and passing as `mut`. Calls allowed.

```rsg
immut p := &Point { x = 1.0, y = 2.0 }
// p.x = 3.0     // ERROR: mutation
// q := p         // ERROR: pointer assignment
do_something(p)   // OK

immut v := Point { x = 1.0, y = 2.0 }
w := v            // OK: value copy

struct Service { immut db: *Database; name: str }
fn create_logger() -> immut *Logger { &Logger { level = "info" } }
```

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
    x := divide(10.0, 2.0)!  // propagates Err
    y := divide(x, 5.0)!
    y
}
```

### Defer & Return

Both are strictly **function-scoped** — unaffected by inner blocks. Defers execute LIFO on function exit.

```rsg
f := open(path)!
defer f.close()
```

### Panic & Recover

`panic` aborts the current execution path. `recover` — only callable inside `defer` — returns `Some(message)` if a panic is active and catches it, `None` otherwise.

When `panic` fires: execution stops, defers run LIFO. If a defer calls `recover()`, the panic is caught and the function returns from the defer (not the panic site).

```rsg
fn safe_call() -> ?str {
    var result: ?str = None
    defer {
        if Some(msg) := recover() {
            result = Some(msg)
        }
    }
    panic("went wrong")  // stops here → defer runs → returns result
    result                // unreachable
}

msg := safe_call()  // Some("went wrong")
```

### Assert

```rsg
assert(x > 0)
assert(x > 0, "x must be positive")
```

---

## 5. Data Modeling

### Structs & Embedding

Value types. Embedding promotes fields/methods; override by redefining.

| Receiver    | Syntax         | Semantics                       |
| ----------- | -------------- | ------------------------------- |
| Value       | `fn f(p)`      | Copy of struct, cannot mutate   |
| Pointer     | `fn f(*p)`     | Read-only pointer (`const T *`) |
| Mut pointer | `fn f(mut *p)` | Mutable pointer (`T *`)         |

```rsg
struct Point {
    x: f64 = 0.0
    y: f64 = 0.0
    fn sum(p) = p.x + p.y                                    // value receiver
    fn set_position(mut *point, target: Point) {             // mut pointer
        point.x = target.x; point.y = target.y
    }
}

struct Point3D {
    Point              // embedding
    z: f64 = 0.0
    fn set_position(mut *p, target: Point3D) {               // override
        p.Point.set_position(x = target.x, y = target.y)
        p.z = target.z
    }
}

a := &Connection { host = "localhost", port = 8080 }
b := a  // pointer copy — same instance
```

### Enums (ADTs)

```rsg
enum Msg {
    Quit,
    Move { x: i32, y: i32 },
    Write(str),
    fn get_str(mut *p) { ... }
}
any := Msg::Write("any")

enum Color { Red, Green, Blue }
enum Status { Active = 1, Inactive = 0, Pending = 2 }
enum Direction { north = "north", south = "south", west = "west", east = "east" }
```

### Pattern Matching

`match` is exhaustive.

```rsg
match msg {
    Quit => println("quit"),
    Move { x, y } => println("{x}, {y}"),
    Write(text) => println(text),
}

match msg {
    Move { x, y } if x > 0 && y > 0 => println("positive"),
    _ => println("other"),
}
```

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

## 6. Abstraction & Extensibility

### Pacts

Require fields/methods, provide defaults. Explicit conformance: `struct Foo: Pact1 + Pact2`.

```rsg
pact A { Ord; Display }       // or: pact A = Ord + Display

pact Animal {
    name: str
    fn get_name(animal) = animal.name
    fn set_name(mut *animal, name: str) { animal.name = name }
}

struct Dog: Animal + Printable {
    fn to_string(dog) = "Dog({dog.name})"
}
```

### Extension Methods

Extend primitives, `array`, `slice`, `tuple`, `struct`, `enum`, `pact`.

```rsg
ext str impl Display {
    fn last_char(*s) -> char { s[s.len() - 1] }
    fn join(*s, sep: str) -> str { ... }
}
```

### Literal Methods

Receiver type determines callability on literals. Value receivers work on rvalues; pointer receivers require an addressable lvalue.

```rsg
ext i32 {
    fn is_even(n) -> bool { n % 2 == 0 }
    fn increment(mut *n) { *n += 1 }
}

10.is_even()         // OK: value receiver on literal
// 10.increment()    // ERROR: pointer receiver on rvalue
x := 10
x.increment()        // OK: variable is addressable

// Struct: same rules
(Counter { value = 0 }).get_value()   // OK: value receiver
// (Counter { value = 0 }).increment()  // ERROR: rvalue
c := Counter { value = 0 }
c.increment()        // OK: implicitly &c
(&Counter { value = 0 }).increment() // OK

ext<T> []T {
    fn len(s) -> usize { ... }
    fn push(mut *s, item: T) { ... }
}
([]i32{1, 2, 3}).len()           // OK: temporary slice copy
(&[]i32{1, 2, 3}).push(4)        // OK
nums := []i32{1, 2, 3}
nums.push(4)         // OK: explicit mutable reference
```

### Generics

Pact-bounded, monomorphized at compile time.

```rsg
fn max<T: Ord>(a: T, b: T) -> T {
    if a > b { a } else { b }
}

pact B<T> { Into<T>; Clone }

fn complex_merge<T: Ord + Display, U: B<T>>(a: []T, b: []U) -> []T { ... }
```

Generic structs, enums, type aliases, and extensions:

```rsg
struct Pair<T, U> {
    first: T
    second: U
    fn map<V>(…) -> Pair<V, U> { ... }
}
Pair<i32, str> { ... } // or infer

enum Either<L, R> {
    Left(L), Right(R)
    fn unwrap_left(*e) -> L { ... }
}

type Callback<T> = fn(T) -> bool
type ListBox = Box<List>

ext<T, U> Pair<T, U> { ... }
ext Pair<i32, i64>  { ... }
```

### Default Generics

Supported on `struct`, `enum`, `pact`, `type` — not `fn`.

```rsg
struct Map<K, V = str> {
    entries: [](K, V)

    fn insert(mut *self, key: K, value: V) { ... }
    fn get(*self, key: K) -> ?V { ... }
}

// V defaults to str when omitted
config := Map<str> { entries = [] }
config.insert("host", "localhost")

// Override the default explicitly
scores := Map<str, i32> { entries = [] }
scores.insert("alice", 100)

enum Result<T, E = str> { Ok(T), Err(E) }
type StringMap<V = str> = Map<str, V>
```

### `Self` & `*Self`

`Self` refers to the concrete type inside `struct`, `enum`, `pact`, `ext` blocks.

```rsg
struct Point {
    x: f64 = 0.0
    y: f64 = 0.0
    fn origin() -> Self { Self { x = 0.0, y = 0.0 } }
    fn translate(p, dx: f64, dy: f64) -> Self { Self { x = p.x + dx, y = p.y + dy } }
    fn reset(mut *self) { self.x = 0.0; self.y = 0.0 }
}

pact Clonable { fn clone(*self) -> Self }

enum Token {
    Number(f64), Ident(str),
    fn dummy() -> Self { Self::Number(0.0) }
}
```

### Recursive & `comptime` Generics

```rsg
struct Node<T> { value: T; next: ?Node<T> }

struct ArrayWrapper<T, comptime N: usize> { data: [N]T }
wrapper := ArrayWrapper<i32, 5> { data: [1, 2, 3, 4, 5] }
```

### `where` Clauses

Move complex bounds out of the signature for readability.

```rsg
fn merge<T, U>(a: []T, b: []U) -> []T
where
    T: Ord + Display + Clone,
    U: Into<T> + Clone,
{ ... }

struct Registry<K, V>
where K: Hash + Eq + Display, V: Clone,
{ entries: [](K, V) }

pact Transformer<In, Out>
where In: Parseable, Out: Serializable + Display,
{ fn transform(*self, input: In) -> Out }
```

---

## 7. `pact` Associated Types

```rsg
pact Iterator {
    type Item: Clone = i32  // bounded
    type Index = usize        // with default
    fn next(mut *self) -> ?Self::Item
}

struct RangeIter { current: i32; end: i32 }

ext RangeIter impl Iterator {
    type Item = i32           // concretize
    type Alias = str;      // Error：`associated type `Alias` is not a member of Iterator`
    // Index defaults to usize
    fn next(mut *self) -> ?i32 { ... }
}

// Usage: no repeated type params
fn collect<I: Iterator>(iter: *I) -> []I::Item { ... }
```

### Constraints & Equality

```rsg
fn only_i32<I: Iterator<Item = i32>>(iter: *I) {
    // I::Item is guaranteed i32
}

fn same_item<I: Iterator, J: Iterator<Item = I::Item>>(
    i: *I, j: *J
) {
    // I::Item == J::Item enforced at compile-time
}

fn collect<T = i32, I: Iterator<Item = T>>(iter: *I) -> []T {
    // T defaults to i32 if not specified
}
```

### Type Inference

Compiler infers associated types from context — no turbofish needed in most cases:

```rsg
iter := RangeIter { current = 0, end = 2 }

// OK: Compiler infers I = RangeIter, I::Item = i32
val := get_next(mut &iter)

// OK: Works in expressions
first := collect(&iter)[0]  // type: i32

var val: ?str = get_next(mut &iter)  // ERROR: conflicts with I::Item = i32
```

### Option/Result Interaction

```rsg
pact FallibleIterator {
    type Item
    type Error: Display
    
    fn next(mut *self) -> Self::Item ! Self::Error
}

fn first_valid<I: FallibleIterator>(
    mut iter: *I
) -> ?I::Item ! I::Error {
    while Some(val) := iter.next()! {  // ! propagates Err(I::Error)
        if val != default() { return Some(val) }
    }
    None
}
```

### Nested Result Handling

```rsg
pact Parser {
    type Output
    type Err
    
    fn parse(*self, input: str) -> Self::Output ! Self::Err
}

fn chain<P, Q>(
    p: *P, q: *Q, src: str
) -> Q::Output ! Error
where
    P: Parser,
    Q: Parser<Output = P::Output>,
    P::Err: CustomError,   // unified error type
    Q::Err: CustomError,
{
    val := p.parse(src)!   // P::Err propagated
    q.parse(val)!          // Q::Err propagated
}
```

### Boundary Types

```rsg
struct UnitIter: Iterator {
    type Item = ()    // unit type
    count: usize
    fn next(mut *self) -> ?() {
        if self.count > 0 { self.count -= 1; Some(()) }
        else { None }
    }
}

// Usage: iterate for side-effects
for UnitIter { count = 3 } |_| { println("tick") }
```

### Projection & Composition

```rsg
// In signatures
fn process<I: Iterator>(iter: *I) -> []I::Item

// In where clauses
fn clone_all<I>(iter: *I) -> []I::Item
where I: Iterator, I::Item: Clone

// In type aliases
type ItemsOf<I: Iterator> = []I::Item
type First<I: Iterator> = ?I::Item

// In structs
struct Cache<I: Iterator> {
    buffer: []I::Item
    index: usize
}
```

### Conflict Resolution

When two pacts define the same associated type name:

```rsg
pact A { type Value }
pact B { type Value }

struct Dual: A + B {
    type A::Value = i32    // qualified assignment
    type B::Value = str
}
```

---

## 8. Functions & Closures

Expression or block bodies. Named args at call site. No nested functions; closures are lambda-only.

```rsg
fn add(a: i32, b: i32) = a + b

var add_fn: fn(i32, i32) -> i32 = add
var add_lambda: fn(i32, i32) -> i32 = |a: i32, b: i32| a + b

set_name(mut user = &myUser, name = "Bob")
```

### Variadic Arguments

```rsg
fn sum(values: ..i32) -> i32 {
    total := 0
    for values |v| total += v
    total
}

sum(1, 2, 3)           // 6
nums := []i32{1, 2, 3}
sum(..nums)             // 6 (spread)
sum(0, ..nums, 99)     // 105

fn bad(values: ..i32, default: i32) -> i32 {
    // ERROR: variadic must be last parameter
}
```

### Closures

`Fn` (read-only captures), `FnMut` (mutable captures), `fn` is subtype of `Fn`:

```rsg
offset := 10
cb := |x| x + offset    // Fn(i32) -> i32

count := 0
var cb2: FnMut(i32) -> i32 = |x| { count += 1; x + count }
```

### Pipe Operator

`|>` pipes left-hand value as first argument to right-hand function.

```rsg
// equivalent: println(to_upper(trim(input)))
input |> trim |> to_upper |> println
values |> filter(|x| x > 0) |> map(|x| x * 2) |> sum
```

---

## 9. Control Flow

`if`, `match`, `while`, blocks are expressions. Statement-form `if` may omit `else`.

```rsg
result := if x > 10 { x } else { x + 1 }
if condition { do_something() }

loop { if done() { break } }
result := loop {
    counter += 1
    if counter == 10 { break counter * 2 }
}

while condition { do_work() }

for values |v| println(v)
for values |v, i| ...
for 0..10 |i| ...
for start..end |i| ...            // variable bounds
for 0..n * 2 |i| ...              // computed bounds
```

Empty range (`N..N`) → zero iterations. `continue` supported in `loop`, `while`, `for`.

### Pattern Binding (`if` / `while`)

Refutable patterns via `:=`. Binds on match; skips on mismatch.

```rsg
if Some(user) := find_user(id) {
    println("found: {user.name}")
} else {
    println("not found")
}

while Some(line) := reader.next_line()! {
    process(line)
}
```

---

## 10. Modules

Each `.rsg` file = module. Private by default. Parent must declare child via `mod`.

```plain
my_project/
└── src/
    ├── main.rsg          # implicit root module, entry point
    ├── db.rsg            # use src/db
    ├── db/
    │   ├── models.rsg    # use src/db/models
    │   └── queries.rsg   # use src/db/queries
    └── utils.rsg         # use src/utils
```

```rsg
// src/main.rsg
mod db
mod utils

fn main() {
    // Absolute path
    _ := src::db::models::User // or db::models:User
}

use src::db::models::User // or db::models:User
User

// src/db.rsg
mod models
mod queries
```

`pub` applies to `fn`, `struct`, `struct` fields, `enum`, `enum` functions, `type`, `var`, `pact`, `pact` fields, `ext` functions.

```rsg
use std
std::io::read(..)

use std::io
io::read(..)

use std::io::{read, write}
read(..)

use std::io::{read as rd}
rd(..)

pub fn add(a: i32, b: i32) = a + b

pub struct User {
    pub name: str
    age: u32          // module-internal only
}

pact User {
    fn internal_helper(*u) // module-internal only
    pub fn public_api(*u)
}
```

Nested modules:

```rsg
mod a {
    fn private_function() { ... }
    pub fn public_function() { ... }

    pub mod b {
        use super::public_function
        pub fn call() { public_function() }

        pub mod c {
            use super::super::public_function
        }
    }
}

fn main() {
    a::b::call() // or self::a::b::call()
}
```

---

## 11. Syntax Reference

### Keywords

```plain
as       break  comptime  continue  defer  else   enum
false    fn     for       if        immut  impl   loop
match    mod    mut       pact      pub    return struct
true     type   use       var       while  where
```

**Reserved:** `async`, `await`, `macro`, `spawn`.

**Conventions:** `snake_case` identifiers, `PascalCase` types, `SCREAMING_CASE` constants. Comments: `//`, `///`.

### Documentation Comments

`///` attaches to the next declaration. Supports `@param`, `@return`, code blocks.

```rsg
/// Computes the GCD using the Euclidean algorithm.
///
/// @param a  First non-negative integer.
/// @param b  Second non-negative integer.
/// @return   The GCD of `a` and `b`.
fn gcd(a: i32, b: i32) -> i32 {
    if b == 0 { a } else { gcd(b, a % b) }
}

/// A 2D point in Cartesian space.
struct Point {
    /// Horizontal coordinate.
    x: f64 = 0.0
    /// Vertical coordinate.
    y: f64 = 0.0
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

### Operators

| Op            | Meaning                     |
| ------------- | --------------------------- |
| `:=`          | Variable declaration        |
| `::`          | Namespace access            |
| `!` (postfix) | Error propagation           |
| `&`           | Address-of / heap-allocate  |
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
**Modulo** sign matches the dividend.

---

## Appendix: Compiler Pipeline

Written in C17. Compiles `.rsg` source to C17. Future back-ends: C++20, Go, Typescript.

```plain
Source (.rsg)
  ↓
[ pass/lex ]        → Lexer ────────── Tokens (via core/token.h)
  ↓
[ pass/parse ]      → Parser ───────── Untyped AST (repr/ast.h)
  ↓
[ pass/resolve ]    → Resolver ─────── AST + Symbol Bindings & Scopes
  ↓
[ pass/infer ]      → Inferencer ───── AST + Type Equations & Unification
  ↓
[ pass/check ]      → Checker ──────── Fully Typed AST + Validated Constraints
  ↓
[ pass/mono ]       → Monomorphizer ── Specialized Typed AST (Generics resolved)
  ↓
[ pass/lower ]      → Lowering ─────── HIR (repr/hir.h) (Desugared, flat control flow)
  ↓
[ optional passes ] → HIR Passes ───── HIR → HIR (Constant folding, CFA, simplifications)
  ↓
[ pass/cgen ]       → CodeGen ──────── Target Code (C17, C++, Go, TypeScript)
```
