# The Ilang Language Specification

This document defines the Ilang language as implemented by `Ilangc` and run by `IlangVM` (bytecode version 1). It describes what a program means, which programs are errors, and what happens at run time. Every code example was compiled with `Ilangc`; examples marked as errors are rejected by it.

Contents: [1 Overview](#1-overview) · [2 Lexical structure](#2-lexical-structure) · [3 Program structure](#3-program-structure) · [4 Types](#4-types) · [5 Declarations](#5-declarations) · [6 Expressions](#6-expressions) · [7 Statements](#7-statements) · [8 Memory and ownership](#8-memory-and-ownership) · [9 Built-in functions](#9-built-in-functions) · [10 Interop with C](#10-interop-with-c) · [11 Run-time behavior](#11-run-time-behavior) · [12 Limits](#12-limits) · [13 Toolchain](#13-toolchain) · [14 Grammar](#14-grammar)

---

## 1 Overview

Ilang is a small, statically typed, imperative language. A program is a set of functions, structs and global variables. It is compiled by `Ilangc` into a bytecode image (`.ic`) and executed by the register-based `IlangVM`.

Its main characteristics:

- **Static types, no implicit conversions** between numeric types. Integer *literals* are the one flexible case (§6.2).
- **Manual-free memory model without a garbage collector.** Strings, arrays and structs live on a heap; the compiler decides who owns each block and releases it automatically when its owner goes out of scope (§8).
- **Bounds-checked** indexing, with an opt-in `unsafe` block.
- **Tail calls** run in constant stack space (§11.3).
- **C interop** in both directions (§10).

A complete program:

```ilang
struct Point { x: int; y: int; }

fn dist2(p: Point) -> int {
    return p.x * p.x + p.y * p.y;
}

fn main() {
    set p = Point();
    p.x = 3;
    p.y = 4;
    print("distance squared: " + str(dist2(p)) + "\n");
}
```

### Notation

Grammar rules (§14) use EBNF: `a b` sequence, `a | b` choice, `[a]` optional, `{a}` zero or more, `"x"` a literal token. "Error" always means a compile-time error unless it says *run-time*. Quoted compiler and VM messages are shown as the current tools print them; their exact wording may change.

---

## 2 Lexical structure

### 2.1 Source text

A source file is a sequence of bytes. A UTF-8 byte-order mark at the start is ignored. Outside string literals and comments only ASCII characters are allowed; any other byte is an error ("unexpected non-ASCII character"). Lines end with `\n`; `\r` is white space.

White space (space, tab, `\r`, form feed, vertical tab, newline) separates tokens and is otherwise insignificant. **Line breaks have no meaning**: statements end with `;`.

### 2.2 Comments

```ilang
// a line comment, to the end of the line
/* a block comment,
   over several lines */
fn main() { }
```

Block comments do **not** nest. An unterminated block comment is an error.

### 2.3 Identifiers and keywords

An identifier is `[A-Za-z_][A-Za-z0-9_]*`. Identifiers are case-sensitive.

The keywords are:

```text
set  const  fn  struct  if  else  while  for  break  continue  return  true  false  unsafe
```

`extern` and `export` are **not** keywords: they have a meaning only in front of `fn` at the top level and can be used as ordinary names elsewhere. The names of the types and built-in functions (`int`, `string`, `print`, …) are not keywords either; which of them may be redefined is described in §5.5.

### 2.4 Integer literals

An integer literal is a decimal number (`42`) or a hexadecimal number with a `0x` or `0X` prefix (`0xFF`, `0xdeadBEEF`). Its value must be in `0 … 4294967295`; a larger one is an error. There is no sign (a leading `-` is the unary minus operator, §6.5), no digit separator, no octal or binary form, and **no type suffix**: a number directly followed by letters (`12abc`, `5u`) is an error.

An integer literal has no type of its own until it meets one (§6.2).

### 2.5 Float literals

A float literal has digits on both sides of the point, or an exponent, or both: `2.5`, `1e10`, `1.5e-3`, `2E+4`. Neither `.5` nor `5.` is a float literal. A float literal has type `float`.

### 2.6 Character literals

A character literal is a single byte between single quotes: `'a'`, `'\n'`, `'\x41'`. Its type is `byte` and its value is the byte (`'a'` is 97). Two characters (`'ab'`) or none (`''`) are errors; use a string for more than one byte.

### 2.7 String literals

A string literal is a sequence of bytes between double quotes. It may not contain a line break. Escape sequences:

| Escape | Byte |
| --- | --- |
| `\n` `\t` `\r` | line feed, tab, carriage return |
| `\0` | the byte 0 (strings may contain NUL bytes) |
| `\\` `\"` `\'` | backslash, double quote, single quote |
| `\xHH` | the byte with the two hexadecimal digits `HH` |

Any other escape is an error. A string is just bytes: Ilang assigns no character encoding to it, and `len` counts bytes.

### 2.8 Operators and punctuation

```text
{ } ( ) [ ] ; , : . -> = == != < > <= >= << >> + - * / % & | ^ && || ! ~ ?
```

The compound assignments `+= -= *= /= %= &= |= ^= <<= >>=` and the statements `++` and `--` are written as two **adjacent** tokens (`+=`, not `+ =`); see §7.4.

### 2.9 Including files

`@include "path"` (or `@include <path>`) is replaced by the tokens of the named file. It is the only directive. The path is looked up relative to the directory of the file being compiled first, then relative to the including file. A file is included at most once, so include cycles and repeated includes are harmless.

---

## 3 Program structure

A program is a sequence of **top-level items**, in any order:

| Item | Form | Section |
| --- | --- | --- |
| function | `fn name(params) -> type { … }` | §5.3 |
| exported function | `export fn name(params) -> type { … }` | §10.2 |
| C function declaration | `extern fn name(params) -> type;` | §10.1 |
| struct | `struct Name { field: type; … }` | §5.4 |
| global variable | `set name = value;` | §5.2 |

Statements are not allowed at the top level.

Functions and structs can be used before their declaration in the file. Global variables are initialized in source order: an initializer can use only the globals declared above it, while every function can use all of them.

### 3.1 Starting a program

Execution happens in this order:

1. the global variables are initialized, in source order;
2. `main` is called;
3. the program ends.

`main` is required. It takes no parameters; its result, if any, is ignored. A program that only provides `export fn`s to a C host may leave `main` out (§10.2).

Functions that `main` (or an export, or an initializer) cannot reach are still type-checked, but are not included in the bytecode.

```ilang
fn log(msg: string) { print("[log] " + msg + "\n"); }

fn start() -> int { log("computing"); return 42; }

set answer = start();   // runs before main

fn main() {
    log("main");
    print(answer);
    print("\n");
}
```

This prints `[log] computing`, `[log] main` and `42`, in that order.

---

## 4 Types

### 4.1 The types

| Type | Values | Size in memory |
| --- | --- | --- |
| `int` | 32-bit signed integer, −2147483648 … 2147483647 | 4 bytes |
| `uint` | 32-bit unsigned integer, 0 … 4294967295 | 4 bytes |
| `float` | 32-bit IEEE 754 binary floating point | 4 bytes |
| `bool` | `true` or `false` | 1 byte |
| `byte` | unsigned 8-bit integer, 0 … 255 | 1 byte |
| `string` | a sequence of bytes | heap block |
| `T[]` | an array of `T`, where `T` is `int`, `uint`, `float`, `bool`, `byte`, `address` or itself an array | heap block |
| *`Name`* | a user-defined struct | heap block |
| `address` | an opaque host value; no literals, no operations (§4.7) | 4 bytes |
| `void` | no value; only as a function's return type | — |

`int`, `uint`, `float` and `byte` are the **numeric** types; `int`, `uint` and `byte` are the **integer** types.

Two types are the same type only if they are written the same way. `string` and `byte[]` are the single exception: they have the same representation and either may be used where the other is expected (§4.4).

### 4.2 Default values

A newly created array is filled with zeros of its element type (`0`, `0.0`, `false`). A new struct has all its fields zeroed (numbers `0`, `false`, empty string). A function that ends without a `return` returns the zero of its return type (§5.3).

### 4.3 Numeric ranges and overflow

Integer arithmetic wraps around (two's complement): `2147483647 + 1` is `-2147483648`, a `uint` that goes below 0 wraps to 4294967295, and a `byte` that goes past 255 wraps modulo 256. Overflow is never an error.

Integer division truncates toward zero (`-7 / 2` is `-3`, `-7 % 3` is `-1`). `INT_MIN / -1` gives `INT_MIN` and `INT_MIN % -1` gives `0`. Division or modulo by zero is a **run-time error**, also for floats (`x / 0.0` panics).

### 4.4 Strings

A string is a block of bytes with a length. It carries no terminator and no encoding. It can be indexed (`s[i]` is a `byte`), measured (`len(s)`), compared, and concatenated with `+` (§6). An element of a string can be assigned (`s[0] = 'J';`). There is no slice operator; use `substr`.

Because `string` and `byte[]` are interchangeable, a string can be passed to a function taking `byte[]` and the other way round. Both names then refer to the same memory (§8).

```ilang
fn main() {
    set s = "AB";
    set b: byte[] = s;      // the same bytes
    b[0] = 'C';
    print(s);               // prints CB
}
```

### 4.5 Arrays

`T[]` is an array whose elements all have type `T`. `T` may be a numeric type, `bool`, `byte` or `address`, or an array, giving arrays of arrays: `int[][]`. Arrays of strings or of structs **do not exist** (error "arrays of 'string' are not supported").

An array knows its length (`len(a)`), which is fixed when it is created. An array of arrays owns its rows, which are separate blocks and may have different lengths (`[[1, 2], [3, 4, 5]]`).

The type is written without a size: `int[10]` is an error. The size is part of the value (§6.9).

### 4.6 Structs

A struct is a record of named fields. A field has a scalar type or the type `string`: **fields cannot be structs or arrays**. Fields are stored in declaration order at their natural alignment (`bool` and `byte` take 1 byte, everything else 4) and the struct is padded to a multiple of 4 bytes. An empty struct is allowed.

### 4.7 `address`

`address` is the type of raw VM addresses. It exists for the interface to C: a host function may declare parameters or results of this type. Ilang code can store, pass, return and compare (`==`, `!=`) such values but cannot create or inspect them, and there is no literal for it.

### 4.8 Conversions

There are **no implicit conversions** between types, with these exceptions:

- an integer literal takes the type of its context (§6.2);
- `string` and `byte[]` are interchangeable.

Everything else is explicit, using the conversion functions of §9.2: `int(x)`, `uint(x)`, `float(x)`, `bool(x)`, `byte(x)`, `str(x)`.

```ilang-error
fn main() {
    set a: int = 1;
    set b: float = 2.5;
    print(a + b);          // error: operand types differ: 'int' and 'float'
}
```

---

## 5 Declarations

### 5.1 Scope

Names declared in a function live in **blocks**: the function body, an `if`/`else`/loop body, a bare `{ }` block, an `unsafe` block, and the header of a `for` statement each start a scope. A name is visible from its declaration to the end of its block. A name cannot be used before its declaration.

- Redeclaring a name **in the same block** is an error. The parameters of a function are in the function's outermost block.
- A name declared in an **inner** block may shadow one from an outer block, and a local may shadow a global.

```ilang
set g = 10;

fn main() {
    set x = 1;
    {
        set x = 2;            // shadows the outer x
        print(x);             // 2
    }
    print(x);                 // 1
    set g = 5;                // shadows the global g
    print(g);                 // 5
}
```

Functions, structs and global variables share the global scope of the program; two functions cannot have the same name, a function cannot have the name of a struct, and so on.

### 5.2 Variables

```text
set name = value;
set name: type = value;
set const name = value;
```

A variable **must be initialized** where it is declared. If the type is omitted it is taken from the value: an integer literal gives `int`, a float literal `float`, and so on. With an annotation, the value must have that type (an integer literal adapts, §6.2).

```ilang
fn main() {
    set a = 7;                    // int
    set b: uint = 4000000000;     // the literal adapts
    set c: float = 3;             // 3.0
    set d = 'x';                  // byte
    set e: int[] = [];            // an empty array needs its type
    print(a); print(b); print(c); print(d); print(len(e));
}
```

`set const` makes the **name** immutable: it cannot be assigned, and if it holds a string, array or struct, its elements or fields cannot be assigned through that name either. (Const is a property of the name: a const array passed to a function is received by an ordinary parameter, which can modify it.)

```ilang-error
fn main() {
    set const limit = 10;
    limit = 11;                   // error: cannot assign to 'limit': it is declared const
}
```

A variable declared with `set` and a value that is an expression of a heap type is subject to the ownership rules of §8.

### 5.3 Functions

```text
fn name(param, param, …) -> type { body }
```

A parameter is `name: type`. A parameter without a type is an `int`, and so is a function without `->`:

```ilang
fn half(x) { return x / 2; }                 // fn half(x: int) -> int
fn area(w: float, h: float) -> float { return w * h; }
fn greet(name: string) -> void { print("hello " + name + "\n"); }

fn main() {
    print(half(9));
    print(area(2.0, 1.5));
    greet("you");
}
```

- `-> void` marks a function that returns nothing. Its call is a statement, not a value; using it as a value is an error ("this expression has no value"). `return;` ends it early, `return value;` is an error. `print` is also a void function.
- A non-void function returns with `return value;`. If control reaches the end of the body without `return`, or at a bare `return;`, the function returns the **zero** of its type (`0`, `0.0`, `false`). A `string`, array or struct result has no such value: it must be returned explicitly on every path, because the missing value is a null block that stops the program when it is used.
- Arguments are evaluated left to right. Scalars (`int`, `float`, `bool`, …) are passed by value: assigning to a parameter changes only the local copy. Strings, arrays and structs are **passed by reference**: the callee gets the same block, can modify its elements or fields, but does not own it (§8).
- A function may be called before its declaration and may call itself or others recursively.
- Ordinary functions have no small limit on their parameters; `extern` and `export` functions are limited to 64 (§10).

### 5.4 Structs

```ilang
struct Account {
    id: int;
    owner: string;
    balance: float;
    active: bool;
}

fn main() {
    set a = Account();            // every field starts as zero / empty
    a.id = 7;
    a.owner = "Ada";
    a.balance += 12.5;
    print(a.owner + ": ");
    print(a.balance);
}
```

A struct value is created with its **constructor**, written like a call with **no arguments**: `Account()`. Fields are then assigned one by one. `value.field` reads a field and `value.field = e;` assigns it.

A `string` field is owned by the struct (§8.4): assigning to it releases the old string.

The following are errors: a constructor call with arguments, an unknown field, a field of struct or array type, a duplicate field, comparing two structs with `==`, and using a field of a call result (`make().x`: store the struct in a variable first).

### 5.5 Reserved and shadowable names

| Names | Rule |
| --- | --- |
| `print` `len` `int` `uint` `float` `bool` `byte` | **Reserved**: they cannot be the name of a function or a struct. |
| `string` `address` `void` | Cannot be the name of a struct. |
| `input` `eof` `str` `chr` `substr` `find` `is_int` `is_uint` `is_float` `copy` `fopen` `fclose` `fread` `freadline` `fwrite` `feof` `file_error` `read_file` | The **library functions**. A function or struct of the program with the same name replaces them. |

When a name is called, the compiler tries, in this order: the reserved names, struct constructors, the program's functions (including `extern fn`s), then the library functions.

### 5.6 Global variables

A top-level `set` declares a global, with the same syntax as a local (`set const` is allowed). Globals are visible in every function. They are initialized before `main`, in source order, and an initializer can use only the globals declared above it. If an initializer calls a function that reads a global declared later, it sees the zero value.

Globals of heap type (string, array, struct) are created once and live until the program ends. Their contents can be changed in place (`table[0] = 1;`, `config.debug = true;`) but they **cannot be reassigned**. There can be at most `stack / 2` (160) globals.

```ilang
set counter = 0;
set table = [1, 2, 3];
set const LIMIT = 100;
set limit2 = LIMIT * 2;

fn bump() {
    counter += 1;
    table[0] += 10;
}

fn main() {
    bump(); bump();
    print(counter); print(" "); print(table[0]); print(" "); print(limit2);   // 2 21 200
}
```

---

## 6 Expressions

### 6.1 Precedence

From loosest to tightest. All binary operators are left-associative; `?:` is right-associative.

| Level | Operators | Notes |
| --- | --- | --- |
| 1 | `? :` | conditional |
| 2 | `\|\|` | logical or |
| 3 | `&&` | logical and |
| 4 | `\|` | bitwise or |
| 5 | `^` | bitwise xor |
| 6 | `&` | bitwise and |
| 7 | `==` `!=` | equality |
| 8 | `<` `>` `<=` `>=` | ordering |
| 9 | `<<` `>>` | shifts |
| 10 | `+` `-` | additive |
| 11 | `*` `/` `%` | multiplicative |
| 12 | `-` `!` `~` (prefix) | unary |
| 13 | `.` `[ ]` `( )` | field, index, call |

This is the order of C, with one difference in effect: since bitwise operators bind looser than `==`, `a & 1 == 0` means `a & (1 == 0)` and is a type error; write `(a & 1) == 0`. A shift binds looser than `+`: `1 << n - 1` is `1 << (n - 1)`.

`=` is not an operator, and neither are `+=`, `++` or `--`: they are statements (§7.4).

### 6.2 Integer literals take their type from context

An integer literal such as `5` has no type until it meets one:

- next to a typed operand it takes that operand's type: `x + 1` with `x: float` is a float addition; `b & 0xF` with `b: byte` is a byte operation;
- in an initializer, assignment, argument or `return` it takes the expected type;
- otherwise it is an `int`.

A literal that does not fit the type it ends up with is an error (`set b: byte = 256;` is "integer literal 256 does not fit in 'byte'"). Constant arithmetic on literals is exact until the result meets a type, so `set u: uint = 3000000000 + 1000000000;` is valid and `set i = 3000000000;` is an error.

A float literal never adapts: `set n: int = 2.5;` is an error. A variable never adapts either.

```ilang
fn main() {
    set f: float = 3;             // literal 3 becomes 3.0
    set x = f * 2;                // literal 2 becomes 2.0
    set b: byte = 200;
    b = b + 100;                  // wraps to 44
    set u: uint = 5;
    print(x); print(" "); print(b); print(" "); print(u - 10);   // 6.000000 44 4294967291
}
```

### 6.3 Arithmetic

`+ - * / %` operate on two operands of the **same numeric type** and give that type. `%` is not defined for floats. Strings are joined with `+` only (§6.6). For other types (`bool`, arrays, structs) the operators are errors.

Integer overflow wraps (§4.3). Division and modulo by zero stop the program (§11.1). Floating point follows IEEE 754 single precision, except that division by zero is an error.

### 6.4 Bitwise operators and shifts

`& | ^` operate on `int`, `uint` or `byte` operands of the same type. `~x` flips all bits of an `int`, `uint` or `byte` (`~0` is `-1`, and for a `byte` `~b` is `255 - b`).

`a << n` and `a >> n` take an `int`, `uint` or `byte` value and a count of **the same type**, and give the type of `a`. `>>` copies the sign bit for `int` and shifts in zeros for `uint` and `byte`. A count at or above the width of the type (32, or 8 for `byte`) gives 0 (−1 for a negative `int >> n`); a negative count is a run-time error. A *constant* count outside `0 … width−1` is a compile-time error.

```ilang
fn main() {
    set a = 1;
    print(a << 4); print(" ");        // 16
    print(-16 >> 2); print(" ");      // -4
    set u: uint = 4000000000;
    print(u >> 4); print(" ");        // 250000000
    set mask = 0xF0;
    print((mask & 0x30) == 0x30);     // true
}
```

### 6.5 Unary operators

| Expression | Operand | Result |
| --- | --- | --- |
| `-x` | `int` or `float` | the negation (a `uint` or `byte` cannot be negated) |
| `!x` | `bool` or a number | `true` if the operand is `false` or zero |
| `~x` | `int`, `uint`, `byte` | all bits flipped |

### 6.6 Strings

`a + b` joins two strings into a new one. Both sides must be strings (`"n = " + 5` is an error; write `"n = " + str(5)`). Strings are compared with `== != < > <= >=`, byte by byte, then by length: `"ab" < "abc"` and `"b" > "abc"` are true. No other operator is defined on strings.

### 6.7 Comparison

`==` and `!=` work on numbers, `bool`, `address` and strings; `< > <= >=` work on numbers and strings. Both operands must have the same type. The result is `bool`. Arrays and structs cannot be compared (compare their elements or fields); a comparison on `bool` other than `==` and `!=` is an error.

### 6.8 Logical operators and conditions

`&&` and `||` take two conditions and give a `bool`. They **short-circuit**: the right operand is evaluated only if the left one does not decide the result.

A **condition** is a `bool` or a number (zero is false, anything else true). A string, array or struct is not a condition (test `s != ""` or `len(s) > 0`).

`c ? a : b` evaluates the condition `c` and then **only** the chosen branch. Both branches must have the same type (a literal adapts). If the branches produce heap values, both must be new values or both existing ones (§8).

```ilang
fn boom() -> bool { print("boom "); return true; }

fn main() {
    if (false && boom()) { print("not reached"); }
    if (true || boom()) { print("short-circuit "); }
    set n = 5;
    print(n > 3 ? "big" : "small");
    print(" ");
    print(n ? "nonzero" : "zero");
}
```

### 6.9 Array expressions

| Form | Meaning |
| --- | --- |
| `[a, b, c]` | an array of the listed values; all must have the same type; a trailing comma is allowed |
| `[]` | an empty array; its type must come from the context (`set a: int[] = [];`) |
| `[v; n]` | an array of `n` elements, each a copy of `v` |
| `[[1, 2], [3]]` | an array of arrays |
| `[row; n]` | an array of `n` independent deep copies of the array `row` |

`n` in `[v; n]` is any integer expression, evaluated at run time (a negative size is a run-time error).

`a[i]` reads element `i` (zero-based). The index may have type `int`, `uint` or `byte`. It is bounds-checked: an index outside `0 … len(a)-1` stops the program with `index out of bounds`; a negative *constant* index is a compile-time error. `a[i] = v;` writes an element.

For an array of arrays, `g[i]` is the row `i`, **borrowed**: it is valid until the row is replaced or `g` is released. `g[i][j]` reads an element of a row. `g[i] = row;` replaces a row (the old row is released); the new row must be a new array, not one that already has an owner. Use `copy(row)` to store a duplicate.

```ilang
fn main() {
    set a = [1, 2, 3];
    set zeros = [0; 4];
    set flags = [true; 3];
    set grid = [[0; 3]; 2];        // 2 rows of 3, independent
    grid[0][1] = 7;
    a[2] += 5;
    print(len(a)); print(len(zeros)); print(len(flags));
    print(grid[0][1]); print(grid[1][1]);             // 7 0
    set jagged = [[1, 2], [3, 4, 5]];
    print(len(jagged[1]));                            // 3
}
```

### 6.10 Field access, calls and constructors

`value.field` reads a field of a struct variable. A call `f(a, b)` calls a function, built-in or struct constructor. A call whose function does not exist is an error ("unknown function"), even if a variable has that name; there are no function values.

The result of a call can be ignored when it is used as a statement. A new heap value that is ignored is released.

### 6.11 Type conversion expressions

The names of the scalar types are also functions that convert their argument (§9.2): `int(x)`, `uint(x)`, `float(x)`, `bool(x)`, `byte(x)`.

---

## 7 Statements

A function body is a sequence of statements; a **block** is `{ statement… }` and is itself a statement. Braces are **mandatory** on `if`, `else`, `while`, `for` and `unsafe`.

### 7.1 Variable declaration

`set [const] name [: type] = expression;` — see §5.2.

### 7.2 Assignment

```text
name = expression;
name.field = expression;
target[index] = expression;        // target is any array or string expression: a[i], g[i][j], s[i]
```

The value must have the type of the target (an integer literal adapts). A variable that owns a string, array or struct can be assigned only a **new** value (a literal, a constructor, a call result, `copy(…)`, an expression such as `s + "x"`), which replaces and releases the old one; §8 has the full rules. A `const` name cannot be assigned.

### 7.3 `if`

```text
if (condition) { … } else if (condition) { … } else { … }
```

The condition goes in parentheses and must be a bool or numeric (§6.8). `else if` chains are written as shown. A condition containing `=` is rejected ("'=' assigns; it cannot be used inside a condition").

### 7.4 Compound assignment, `++`, `--`

`x += v;` is shorthand for `x = x + v;` and exists for `+ - * / % & | ^ << >>`. `x++;` and `x--;` mean `x += 1;` and `x -= 1;` (also written `++x;` and `--x;`). The target may be a variable, a field (`p.n += 1`) or an array element (`a[i] *= 2`); for an element, the array and the index are evaluated only once.

These are **statements, not expressions**: `y = x++;` and `f(x += 1)` are errors. `++` and `--` must be followed by `;`, `)` or `}`.

### 7.5 `while`

```text
while (condition) { … }
```

### 7.6 `for`

```text
for (init; condition; step) { … }
```

`init` is a `set` statement or any simple statement (assignment, call, …) and `step` is any simple statement except `set`; each of the three parts may be left out (`for (;;)` loops forever). A variable declared by `init` is in a scope that covers the whole loop. `continue` jumps to `step`.

```ilang
fn main() {
    for (set i = 0; i < 3; i++) { print(i); }          // 012
    set j = 10;
    for (; j > 7; j--) { print(j); }                   // 1098
    set n = 0;
    for (;;) {
        n++;
        if (n == 3) { continue; }
        if (n > 5) { break; }
        print(n);                                      // 1245
    }
}
```

### 7.7 `break` and `continue`

`break;` leaves the innermost `while` or `for`; `continue;` goes to its next iteration. Both are errors outside a loop. Values owned by the blocks left are released first (§8).

### 7.8 `return`

`return expression;` ends the function with a value; `return;` ends a `void` function. See §5.3 and, for heap values, §8.

### 7.9 `unsafe`

```text
unsafe { … }
```

Inside an `unsafe` block, array and string **indexing is not bounds-checked**: no length is loaded and no comparison is made. The block is lexical: functions called from it are compiled normally and check their own indexing. An index outside the array reads or writes whatever lies at that address; the VM still refuses addresses that are not inside a live heap block (the program stops), but a neighbouring block can be corrupted silently. Use it only in hot loops whose indices are known to be valid.

```ilang
fn sum(a: int[]) -> int {
    set s = 0;
    unsafe {
        for (set i = 0; i < len(a); i++) { s += a[i]; }
    }
    return s;
}

fn main() { print(sum([1, 2, 3, 4])); }
```

### 7.10 Expression statement

A call (or any expression) followed by `;` is a statement. A bare expression without effect is accepted but useless.

---

## 8 Memory and ownership

Strings, arrays and structs are **heap blocks**. There is no garbage collector and no explicit free: the compiler tracks, at compile time, which variable **owns** each block and inserts the release at every exit from the owner's scope (end of block, `return`, `break`, `continue`).

### 8.1 Fresh values and owners

An expression produces a **fresh** value if it creates a new block: a literal (`"x"`, `[1, 2]`), a constructor (`Point()`), `[v; n]`, a concatenation, a call that returns a heap value, `copy(x)`, `substr`, `str`, `input`, `fread`, `read_file`, …

- `set v = <fresh value>;` makes `v` the **owner** of the block. It is released when `v` goes out of scope.
- `set v = <existing variable or field>;` makes `v` an **alias** (a borrow) of that block. An alias owns nothing and releases nothing.
- **Parameters** are aliases of what the caller passed. A fresh value passed straight to a call (`f("a" + b)`) is released by the caller after the call.

```ilang
fn main() {
    set a = [1, 2, 3];       // a owns the array
    set b = a;               // b is an alias: the same block
    b[0] = 99;
    print(a[0]);             // 99
    set c = copy(a);         // c owns a new, independent copy
    c[0] = 1;
    print(a[0]);             // 99
}
```

### 8.2 Assignment rules

- An **owner** can only be assigned a *fresh* value; the old block is released. `a = [4, 5];` is valid. `a = b;` (an existing value) is an error: "'a' owns its value, so it can only be assigned a newly created one" (it would be released twice). Assign a copy: `a = copy(b);`.
- An **alias or parameter** cannot be assigned a fresh value (it would leak); it can be re-pointed to another existing value.
- A **global** of heap type cannot be reassigned at all (§5.6).
- A **struct** variable cannot be assigned another struct; assign its fields.

### 8.3 Returning heap values

A function may return a heap value only if it owns it: a local created in the function (`return result;` transfers the block to the caller instead of releasing it) or any fresh expression. Returning a parameter, a global, an alias or a field of another struct is an error; return `copy(x)` (for a struct, build a new one and copy the fields).

```ilang
fn squares(n: int) -> int[] {
    set r = [0; n];
    for (set i = 0; i < n; i++) { r[i] = i * i; }
    return r;                // ownership moves to the caller
}

fn greet(name: string) -> string {
    return "Hello, " + name + "!";
}

fn main() {
    set s = squares(5);
    print(s[4]); print(" "); print(greet("Ada"));    // 16 Hello, Ada!
}
```

### 8.4 Struct fields and rows

- A struct **owns its `string` fields**. The constructor sets each to an empty string. Assigning to a string field releases the old string; assigning a value that is not fresh stores a copy. Reading `p.name` gives a borrowed alias (so it cannot be returned directly). Releasing the struct releases its strings.
- An **array of arrays owns its rows**. Releasing it releases every row. A row that already has an owner cannot be stored as a row; store `copy(row)`. `g[i]` reads a row as a borrow.

### 8.5 What the compiler does not check

The compiler does not prove that an alias is still valid. If the owner is reassigned or released while an alias is still used, the alias points to a released block. This does not corrupt the VM: the access is refused and the program stops with a memory error ("LOAD attempted to access unmapped memory…"), but the program is wrong.

```ilang-panic
fn main() {
    set a = [1, 2, 3];
    set b = a;               // alias
    a = [4, 5, 6, 7];        // releases the old block…
    print(b[0]);             // …so this stops the program with a memory error
}
```

---

## 9 Built-in functions

### 9.1 Output and sizes

| Function | Description |
| --- | --- |
| `print(x)` | Writes `x` to standard output, with **no** newline. Accepts any number, `bool`, `byte` or string; one argument only. Arrays and structs cannot be printed. |
| `len(x)` | The length of an array or a string, as an `int`. |

`print` formats values as follows: integers in decimal; a `float` with six digits after the point (`2.5` prints as `2.500000`); `bool` as `true` or `false`; a `byte` as a **number** (`print('A')` prints `65`; print `chr(65)` to print the letter); a string as its bytes. Standard output is flushed before the program reads standard input and when it ends.

### 9.2 Conversions

| Function | Argument | Result |
| --- | --- | --- |
| `int(x)` | `uint`, `byte`, `float`, `bool`, string | `int` |
| `uint(x)` | `int`, `byte`, `float`, `bool`, string | `uint` |
| `float(x)` | `int`, `uint`, `byte`, `bool`, string | `float` |
| `byte(x)` | `int`, `uint`, `float`, `bool` | `byte` |
| `bool(x)` | `int`, `uint`, `float`, `byte` | `bool`: `false` for zero, otherwise `true` |
| `str(x)` | a number, `bool` or `byte` | a new string, formatted as `print` does |
| `chr(b)` | an `int`, `uint` or `byte` in 0 … 255 | a new string of that one byte |

- Integer to integer conversions wrap (`byte(300)` is 44, `uint(-1)` is 4294967295).
- Float to integer conversions truncate toward zero and **saturate** at the limits of the target (`int(1e20)` is 2147483647, `uint(-3.7)` is 0); `int(3.99)` is 3, `int(-3.99)` is -3.
- `int(true)` is 1 and `int(false)` is 0.
- A string is parsed by `int`, `uint` and `float` only. Leading and trailing white space is allowed. `int` accepts an optional sign and decimal digits that fit in an `int`, `uint` accepts digits only, `float` accepts `[+-]digits[.digits][e[+-]digits]` (no `inf`, `nan` or hex). A string that is not a valid number stops the program with "cannot convert the string to a number".
- `chr` with a constant outside 0 … 255 is a compile-time error, with a computed one a run-time error.

### 9.3 Strings

| Function | Description |
| --- | --- |
| `substr(s, start, count)` | A new string of `count` bytes starting at `start`. A range outside the string stops the program. |
| `find(haystack, needle)` | The `int` index of the first occurrence of `needle`, or `-1`. |
| `is_int(s)`, `is_uint(s)`, `is_float(s)` | `true` if `int(s)`, `uint(s)` or `float(s)` would succeed. |
| `copy(x)` | A new, independent copy of a string or array (rows included). |

```ilang
fn main() {
    set line = "  42 apples ";
    set at = find(line, "apples");                 // 5
    set n = int(substr(line, 0, at));              // parsed with surrounding spaces: 42
    print(n + 1); print(" ");                      // 43
    print(is_int("12a")); print(" ");              // false
    print(str(n) + chr(33));                       // 42!
}
```

### 9.4 Standard input

| Function | Description |
| --- | --- |
| `input()` / `input(prompt)` | Flushes standard output, prints the optional prompt, then reads one line from standard input and returns it **without its line ending**. At the end of the input it returns an empty string. |
| `eof()` | `true` if standard input has no more data. Use it to tell the end of the input from an empty line. |

```ilang
fn main() {
    set total = 0;
    while (!eof()) {
        set line = input();
        if (is_int(line)) { total += int(line); }
    }
    print(total);
}
```

### 9.5 Files

A file is an `int` **handle**. Handles `0`, `1` and `2` are standard input, standard output and standard error.

| Function | Description |
| --- | --- |
| `fopen(path, mode)` | Opens a file; `mode` is `"r"` (read), `"w"` (create or empty, write) or `"a"` (append), optionally followed by `b`. Returns a handle, or `-1` if the file cannot be opened. A literal mode that is not one of these is a compile-time error. |
| `fclose(h)` | Flushes and closes; returns `false` if flushing failed. |
| `fread(h, n)` | A new string of up to `n` bytes; shorter only at the end of the file. |
| `freadline(h)` | A new string with one line, without its line ending; empty at the end. |
| `fwrite(h, s)` | Writes the string; returns `true` if all of it was written. |
| `feof(h)` | `true` if no data is left to read. |
| `file_error()` | A new string describing why the last `fopen`, `fclose` or `fwrite` failed, empty if none has. |
| `read_file(path)` | The whole file as a string. A file that cannot be read stops the program. |

A file that the program forgets to close is flushed and closed when the program ends. At most 32 files can be open at once.

```ilang
fn main() {
    set h = fopen("/tmp/ilang_spec_demo.txt", "w");
    if (h < 0) { print(file_error()); return; }
    fwrite(h, "first\nsecond\n");
    fclose(h);

    set r = fopen("/tmp/ilang_spec_demo.txt", "r");
    while (!feof(r)) {
        set l = freadline(r);
        if (l != "") { print("<" + l + ">"); }
    }
    fclose(r);
    fwrite(2, "this goes to standard error\n");
}
```

---

## 10 Interop with C

### 10.1 `extern fn`: Ilang calls C

```text
extern fn name(params) -> type;
```

declares a function implemented in C by a **host library** loaded with `ilangvm --lib`. It has no body. The same rules as for functions apply to parameters and the return type (`int` if `->` is omitted; use `-> void` for none), with these restrictions: no struct may be passed or returned, at most 64 parameters, and the name has at most 255 characters.

The VM refuses to load a program whose declaration does not match the signature the host registered under that name.

```ilang-host
extern fn host_add(a: int, b: int) -> int;
extern fn linspace(lo: float, hi: float, n: int) -> float[];
extern fn host_log(msg: string) -> void;

fn main() {
    host_log("starting");
    print(host_add(2, 3));
}
```

(This program needs a host library that defines the three functions to run; it compiles on its own.)

Strings and arrays are **borrowed** by the host function: it may read and change their contents in place but must not keep a pointer after it returns. A string or array it returns is created by the host during the call. A host function reports a failure by returning an error message, which stops the program (§11.1).

### 10.2 `export fn`: C calls Ilang

```text
export fn name(params) -> type { body }
```

is an ordinary function that a host program may also call by name. The same restrictions as for `extern fn` apply. Exports are always compiled, even when `main` does not call them. A program that has exports may leave out `main`; the host then initializes the program (running the global initializers) and calls the exports itself.

```ilang
export fn compare(a: int, b: int) -> int { return a - b; }

fn main() { print(compare(5, 3)); }
```

The C side (the API a host function sees, the signature strings, and the rules for memory that crosses the boundary) is documented in `IlangVM/VM/IlangHost.h`.

---

## 11 Run-time behavior

### 11.1 Run-time errors

A run-time error stops the program at once: the VM prints `IlangVM Panic! <message>` to standard error and exits with status 1. Open files are flushed. The errors a correct program can cause are:

| Cause | Message |
| --- | --- |
| index outside an array or string | `runtime error: index out of bounds in '<function>'` |
| division / modulo by zero (integers and floats) | `Division by zero!` / `Modulo by zero!` |
| negative shift count | `SHL received a negative shift count!` (`SHR …`) |
| `[v; n]` with a negative `n` | `NEWARRAY requires a non-negative array size!` |
| `substr` range outside the string | `STRSLICE range is outside the string!` |
| `int`/`uint`/`float` of a non-number string | `runtime error: cannot convert the string to a number in '<function>'` |
| `chr` outside 0 … 255 | `STRCHR requires a value from 0 to 255!` |
| `read_file` of an unreadable file | `FREADFILE cannot open '<path>': <reason>` |
| use of a released block (a stale alias, or `unsafe` indexing out of range) | `LOAD attempted to access unmapped memory …` |
| too deep non-tail recursion | `Stack overflow on PUSH!` or `Call stack overflow!` |
| a host function reports an error | `host function '<name>': <message>` |

Output written before the error is not lost.

### 11.2 Evaluation order

Operands, arguments and array elements are evaluated **left to right**. `&&`, `||` and `?:` evaluate only what they need. Global initializers run in source order before `main`.

### 11.3 Tail calls

A call in tail position becomes a jump, so tail recursion runs in constant stack space. The tail positions are:

- `return f(…);` in any function;
- in a `-> void` function, a call that is the last thing the function does: `f(…);` at the end of the body, before a bare `return;`, or at the end of an `if`/`else`/block that is itself in that position.

Tail calls also work between different functions (mutual recursion). A call stays an ordinary call when the callee is an `extern fn` or returns a different type than the caller, when a *different* function would need some of its arguments on the stack (more than fit in registers), or when a string, array or struct argument would not survive the release of the caller's frame: a freshly created temporary, or anything that belongs to a local variable. Only heap values reached from a parameter or a global can be passed on. A function without `->` returns `int`, so its trailing plain call `f(…);` is not a tail call; write `return f(…);` or declare `-> void`.

```ilang
fn count(n: int, acc: int) -> int {
    if (n == 0) { return acc; }
    return count(n - 1, acc + 1);       // a jump: runs 3 million times in constant space
}

fn even(n: int) -> bool { if (n == 0) { return true; } return odd(n - 1); }
fn odd(n: int) -> bool { if (n == 0) { return false; } return even(n - 1); }

fn main() {
    print(count(3000000, 0)); print(" ");
    print(even(1000001));
}
```

Other recursion needs a stack frame per level (§12). Write it with an accumulator to make it a tail call.

---

## 12 Limits

| Limit | Value | Notes |
| --- | --- | --- |
| Integer literal | 4294967295 | |
| Errors reported per compile | 20 | then "stopped after 20 errors" |
| Non-tail recursion depth | about 300 levels | `MAX_STACK_SIZE` (320 values) and `MAX_CALL_DEPTH` (1280 entries) in `IlangConf.h`; a build setting of the VM |
| Global variables | 160 | half the data stack |
| Parameters of `extern`/`export` functions | 64 | |
| Distinct `extern` functions in a program | 65535 | |
| Open files | 32 | |
| Registers | 24 | `--regs 8…64`; must match the VM build |

A large expression that needs more registers than are free is spilled to the stack automatically; only with a very small register file can it fail with "expression too complex: it needs more registers than the VM has".

The heap of a program is limited by the VM to just under 4 GiB in total (`ILANG_VM_MEMORY_LIMIT`).

---

## 13 Toolchain

### 13.1 Compiling

```text
ilangc <input.i> [-o output.ic] [options]
```

| Option | Meaning |
| --- | --- |
| `-o file` | Output file (default: the input with the extension `.ic`). |
| `--regs N` | Number of VM registers, 8 … 64 (default 24). Must equal the VM's `REG_COUNT`. |
| `--temps N` | Registers kept for expression temporaries (default `max(4, regs/3)`). |
| `--zero-init` | Explicitly zero struct fields (redundant). |
| `--strict-tags` | For VMs built with `ILANG_VM_STRICT_TYPES`. |
| `-v` | Print compile statistics. |

Exit status: 0 success, 1 compile errors, 2 bad usage. Errors are printed with the source line, a marker under the offending token, notes pointing at related declarations, and often a `help:` line with the likely fix:

```text
prog.i:2:21: error: type mismatch in argument 1 of 'f': expected 'int', found 'string'
   2 | fn main() { print(f("x")); }
     |                     ^~~
prog.i:1:6: note: parameter 'a' is declared here as 'int'
   1 | fn f(a: int) -> int { return a; }
     |      ^
  help: use int(...) to parse the string as a number

1 error found
```

The compiler reports as many independent errors as it can in one run.

### 13.2 Running and inspecting

```text
ilangvm [--lib <library>]... <file.ic>
ilangdis [options] <file.ic>
```

`ilangvm` runs the program (`--lib` loads host libraries for `extern fn`s). Exit status is 0 if the program ran to the end and 1 on a load or run-time error; error messages go to standard error. `ilangdis` prints the bytecode as a listing. The bytecode format and the instruction set are documented in `IlangVM/VM/IlangVM.h`.

---

## 14 Grammar

```text
program      = { item } ;
item         = function | "extern" "fn" signature ";" | "export" function
             | struct | global ;

function     = "fn" signature block ;
signature    = ident "(" [ param { "," param } ] ")" [ "->" type ] ;
param        = ident [ ":" type ] ;
struct       = "struct" ident "{" { ident ":" type ";" } "}" ;
global       = "set" [ "const" ] ident [ ":" type ] "=" expr ";" ;
type         = ident { "[" "]" } ;                      (* "void" only as a return type *)

block        = "{" { statement } "}" ;
statement    = simple ";" | if | while | for | block | "unsafe" block
             | "break" ";" | "continue" ";" | "return" [ expr ] ";" ;
simple       = "set" [ "const" ] ident [ ":" type ] "=" expr
             | ident "=" expr
             | ident "." ident "=" expr
             | expr "[" expr "]" "=" expr
             | target ( "+=" | "-=" | "*=" | "/=" | "%=" | "&=" | "|=" | "^=" | "<<=" | ">>=" ) expr
             | target ( "++" | "--" ) | ( "++" | "--" ) target
             | expr ;
target       = ident | ident "." ident | postfix "[" expr "]" ;
if           = "if" "(" expr ")" block [ "else" ( if | block ) ] ;
while        = "while" "(" expr ")" block ;
for          = "for" "(" [ simple ] ";" [ expr ] ";" [ simple ] ")" block ;

expr         = binary [ "?" expr ":" expr ] ;
binary       = unary { binop unary } ;                  (* precedence as in section 6.1 *)
binop        = "||" | "&&" | "|" | "^" | "&" | "==" | "!=" | "<" | ">" | "<=" | ">="
             | "<<" | ">>" | "+" | "-" | "*" | "/" | "%" ;
unary        = ( "-" | "!" | "~" ) unary | postfix ;
postfix      = primary { "." ident | "[" expr "]" } ;
primary      = integer | float | string | char | "true" | "false"
             | ident | ident "(" [ expr { "," expr } ] ")"
             | "(" expr ")"
             | "[" "]" | "[" expr { "," expr } [ "," ] "]" | "[" expr ";" expr "]" ;
```

The `simple` forms with `set` are not allowed in a `for` step. A `simple` statement of the form `expr` is normally a call.
