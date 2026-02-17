Pointer-, reference- and slice types

```sl
// Value types
T     // one value
[T N] // array of N values

// Pointer types (owns the memory it points to)
*T   // ptr to value
*[T] // ptr to array with len

// Reference types (does not own the memory it points to)
&T          // ref to value
&[T N]      // ref to array
&[T]        // slice of array w cap,len
const&T     // read-only ref to one value
const&[T N] // read-only ref to array of N values
const&[T]   // read-only slice of array w len
```

Examples:

```sl
var v1 i32 = 1
var v2 [i32 3]; assert len(v2) == 3

var p1 *i32   = new(i32);    assert *p1 == 0
var p2 *[i32] = new(i32, 3); assert len(p2) == 3

var r1 &i32     = &v1; assert r1 == 1
var r2 &[i32 3] = &v2; assert len(r2) == 3
var r3 &[i32]   = &v2; assert len(r3) == 3
var r4 &[i32]   = &v2[1:]; assert len(r4) == 2
```

Type conversions

```sl
*T     → &T          // explicit; requires 'x as &T'
*T     → const&T     // explicit; requires 'x as const&T' or 'x as &T'
*T    ←→ *[T 1]      // explicit; requires 'x as *[T 1]'
*T     → &[T 1]      // explicit; requires 'x as &[T 1]'
*T     → const&[T 1] // explicit; requires 'x as const&[T 1]' or 'x as &[T 1]'
&T     → const&T     // implicit
&T    ←→ &[T 1]      // implicit
&T     → const&[T 1] // implicit
[T N]  → &[T N]      // implicit
[T N]  → &[T]        // implicit
[T N]  → const&[T N] // implicit
[T N]  → const&[T]   // implicit
&[T N] → &[T]        // implicit
&[T N] → const&[T]   // implicit
*[T N] → &[T N]      // implicit
*[T N] → const&[T N] // implicit
&[T]   → const&[T]   // implicit
```

Representation; memory layout

```sl
[T N]:     { a, b … n T }
*[T]:      { addr usize; len usize }
&[T]:      { addr usize; len usize; cap usize; }
const&[T]: { addr usize; len usize }
```

Array literals

```sl
var v1 [i32 3] = {1, 2, 3}
var v2 [i32 _] = {1, 2, 3} // actual type is [i32 3]
var v3         = {1, 2, 3} // actual type is [i32 3]
```

String literals

```sl
// a string literal is a read-only reference to an array of bytes
var s1 const&[u8] = "hello"
var s2 *[u8] = str_concat(s1, " world")
var s3 &[u8] = s2[6:] // slice of s2 ("world")

// make 'str' a distinct alias of '[u8]' to mean "UT8-data"
type str [u8]
var s4 const&str = "hello"
var s5 *str = str_concat(s1, " world")
var s6 &str = s5[6:] // slice of s5 ("world")
```

Alternatives for a `str` type, which means "UTF-8 data"

```sl
// Alt 1: make 'str' a distinct alias of '[u8]'
type str [u8]
var s4 const&str = "hello"
var s5 *str = str_concat(s1, " world")
var s6 &str = s5[6:] // slice of s5 ("world")
fn log(message const&str) void

// Alt 2: make 'str' special type where
//     'str'  = 'const&[u8]'
//     '*str' = '*[u8]'
//     '&str' = '&[u8]'
var s4 str = "hello"
var s5 *str = str_concat(s1, " world")
var s6 &str = s5[6:] // slice of s5 ("world")
fn log(message str) void
```
