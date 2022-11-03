# Simple Map Filter Language

The “Simple Map Filter Language” – short “simfil” – is a query language for
structured data built for use with GeoJSON.

## Language Basics

### Syntax

The syntax of simfil is case-insensitive. This means that `any(...)`, `ANY(...)` and `Any(...)` all compile
to the same function.

### Paths

To traverse the document and evaluate nested nodes, simfil provides the
path operators `.`, `*` and `**`, the latter two acting as a wildcards (direct child or recursive).

The expression `a.b` evaluates the left side (`a`) and, if it matches the current
node, the right one (`b`), resulting in the value of node “b” of node “a”.

A wildcard expression `a.**.b` does the same but `b` must not be a direct sub-node
of `a` but can occur anywhere bellow `a`. Note that this can match multiple nodes!
The result of wildcard expressions depends on the current execution mode they are
executed under (see [Modes](#Modes) ).

The path `_` (underscore) represents the current node.
The path `*` (asterisk) represents any direct child node.
The path `**` (double asterisk) represents any child (recursive) plus the current element.

### Sub-Queries

Sub-queries can be written as a brace-enclosed expression. Path modifications inside
a sub-query will not propagate to the outside. If the sub-query evaluates to a non-false result,
the value of its left side expression will be returned.

A basic sub-query could look like the following:
```
a.b{c}
```

Or (with the optional path separator):
```
a.b.{c}
```

Further path elements can follow:
```
a.b{c}.d
  ^
  Here, b is returned if c is non-false; null otherwise.
```

In the latter case, the value of `d` will only be returned if the sub-expression returned a non-false
value.

Note that the current node in and after the sub-query is the same (`b` in the examples given).

### Arrays

To check the existence of the string `"hello"` inside an array `b` you could write (note: `*` returns every direct child):
```
a.b.*{_ == "hello"}
```

## Modes

### Any

The default mode `any` returns the first non-false nodes value.
```
any(a.**.b{c})
```

Will return `true` if the query evaluates to non-false for at least one node.

### Each / All

The mode `each` requires each node to match its query to return `true`.
```
each(a.**.b{c})
```

Will return `true` if each found `b{c}` evaluates to a non-false value.
Considering the following document, the function will return `true`.

```js
{
  a {
    b {
      c = true ← OK: a.b.c = true
    },
    x {
      b {
        c = true ← OK: a.x.b.c = true
      }
    }
  },
  ...
}
```

`all` is an alias for `each`.

### Count

Count can be used to count matching nodes. The function evaluates to the number
of non-false evaluations of its query.

```
count(a.b.**.c) ← Counts all non-false c bellow "a.b"
```

To count the items of a list you can use the following code:
```
count(mylist.*)
```

## Types

Simfil supports the following scalar types: `null`, `bool`, `int`, `float` (double precision) and `string`.
Additionally, the `model` type represents compound object/array container nodes.
All values but `null` and `false` are considered `true`, implicit boolean conversion takes place for operators
`and` and `or` only.

Functions can return values of types other than the ones mentioned. See [Functions](#Functions) for details.

## Type Casting

Types values can be cast/interpreted to a different type using the `as` operator.
The following types can be target types for a cast:
* `bool` - See operator [`?`](#Operators) and [Types](#Types).
* `int` - Converts the value to an integer. Returns 0 on failure.
* `float` - Converts the value to a float. Returns 0 on failure.
* `string` - Converts the value to a string. Boolean values are converted to either "true" or "false".

## Operators

| Operator            | Function                                                                                                |
|---------------------|---------------------------------------------------------------------------------------------------------|
| `[ a ]`             | Array/Object subscript, index expression can be of type `int` or `string`.                              |
| `{ a }`             | Sub-Query (inside sub-query `_`  represents the value the query is applied to).                         |
| `. b` or `a . b`    | Direct field access; returns the value of field `b` or `null`.                                          |
| `a as b`            | Cast a to type b (one of `bool`, `int`, `float` or `string`).                                           |
| `a ?`               | Get boolean value of `a` (see ##Types).                                                                 |
| `a ...`             | Unpacks `a` to a list of values (see function `range` under [Functions](#Functions) for example)        |
| `typeof a`          | Returns the type of the value of its expression (`"null"`, `"bool"`, `"int"`, `"float"` or `"string"`). |
| `not a`             | Boolean not.                                                                                            |
| `# a`               | Returns the length of a string or array value.                                                          |
| `~ a`               | Bitwise not.                                                                                            |
| `- a`               | Unary minus.                                                                                            |
| `a * b`             | Multiplication.                                                                                         |
| `a / b`             | Division.                                                                                               |
| `a % b`             | Modulo.                                                                                                 |
| `a + b`             | Addition or string concatenation.                                                                       |
| `a - b`             | Subtraction.                                                                                            |
| `a << b` / `a >> b` | Bitwise left shift / bitwise right shift                                                                |
| `a & b`             | Bitwise and.                                                                                            |
| `a \| b`            | Bitwise or.                                                                                             |
| `a ^ b`             | Bitwise XOR.                                                                                            |
| `a < b` / `a <= b`  | Less than / less than or equal to.                                                                      |
| `a > b` / `a >= b`  | Greater than / greater than or equal to.                                                                |
| `a == b` / `a != b` | Equal to / not equal to. `a = b` is an alias for `a == b`.                                              |
| `a =~ b` / `a !~ b` | `a` matches regular expression `b` / `a` does not match regular expression `b`. Returns `a` or `false`  |
| `a or b`            | Logical or, returning the first non-false argument (like JavaScript).                                   |
| `a and b`           | Logical and, returning the first false argument (like JavaScript).                                      |

### Precedence

| Operators                              | Precedence |
|----------------------------------------|------------|
| `.`                                    | 12         |
| `{}`                                   | 11         |
| `[]`                                   | 10         |
| `?`, `...`                             | 9          |
| `typeof`, `not`, `#`, `~`, `-` (unary) | 8          |
| `as`                                   | 7          |
| `*`, `/`, `%`                          | 6          |
| `+`, `-`                               | 5          |
| `<<`, `>>`, `&`, `\|`, `^`             | 4          |
| `<`, `<=`, `>`, `>=`                   | 3          |
| `==`/`=`, `!=`, `=~`, `!~`             | 2          |
| `and`, `or`                            | 1          |

## Functions

### `trace(expr, limit=<...>, name=<...>)`

Counts and measures all calls to its expression under the identifier `name` or the string
representation of `expr`, if no name is given. Returnts the value of `expr`. Result values
of `expr` are stored for debugging reasons; see `limit`.

*Example*
```
trace(a.**.b{trace("sub", c == "test")})
```

Arguments:
- `expr` Expression to trace
- `limit` Limit of values of `expr` to store with the trace entry
- `name` Human readable name of the trace entry; defaults to the string repr. of `expr`.

### `range(begin, end)`

The function returns a value of type `irange` which overloads the following operators:
- `==` Tests if an element is insides the range
- `!=` Tests if an element is outsides the range
- `...` Unpacks the range to its values (e.G. `range(1,5)... => {1, 2, 3, 4, 5}`)
- `string` Converts the range to a string representation (`'begin..end'`)

*Example*
```
range(1, 5) => 1..5

typeof range(1, 5) => 'irange'
range(1, 5) as string => '1..5'
```

### `arr(...)`

Returns its arguments.
```
arr(1, 2, 3) => {1, 2, 3}
```

*Example*
```
v == arr(1, 5, 11) ← Results in three comparisons, one against each element
```

### `split(str, sep, keep_empty=true)`

Splits a string into sub-strings
```
split('hello world', ' ')
```

*Example*
```
split('hello world', ' ') => 'hello', 'world'
```

### `select(values..., index, count=1)`

Selects a slice of values
```
select(arr('a', 'b', 'c'), 1)
```

Index is zero based!
If `count` is <= 0, all trailing values are returned.

*Example*
```
select(arr('a', 'b', 'c'), 1) => 'b'
select(arr('a', 'b', 'c'), 1, 2) => 'b', 'c'
select(arr('a', 'b', 'c'), 0, 0) => 'a', 'b', 'c'
```

### `sum(values..., expr=$sum + $val, init=0)`

Returns the sum of all values `values`. Uses the expression `expr` if given.
Initial value of `$sum` is set to `init`. The result value of `expr` is stored
into `$sum`, which then gets returned by the function. The zero based index of the current
element is accessible as `$idx`.

Note: `$sum`, `$val` and `$idx` are pseudo-fields injected into the current context.

*Example*
```
sum(range(1, 10)...) => 55
sum(range(1, 10)..., $sum * $val, 1) => 3628800

-- Joining a list of strings with ', '
sum(list, #$sum > 0 and $sum + ', ' + $val or $val, '')
```


### `keys(object)`

Returns all sub-element keys of object `object`

*Example*
```
keys(a.b) => 'c', 'd', ...
```

# Working with GeoJSON

The query language has native support for GeoJSON types.
GeoJSON objects implement the following operators:
- `within` To check whether the left value is enclosed by the right object
- `contains` To check if the left value contains the right value
- `intersects` To check if two objects intersect

*Example*
```
-- Check if a bbox contains a point
bbox(0, 0, 10, 10) contains point(1, 1) => true

-- Check whether the current feature intersects a bounding box
geo() intersects bbox(11, 11, 12, 12)
```

## Functions

### `geo([obj])`

Returns the GeoJSON object at `obj` (defaults to `_`).
The following types are supported:
- Point
- LineString
- Polygon

The following types are returned as multiple values:
- MultiPoint
- MultiLineString
- MultiPolygon

### `point(x, y)`

Returns a GeoJSON `point` object.

### `bbox(x1, y1, x2, y2)`

Returns a bounding box `bbox` object.

### `linestring(<x, y>...)`

Returns a GeoJSON `linestring` object.

### `polygon(<x, y>...)` NOT YET IMPLEMENTED

Returns a GeoJSON `polygon` object.
