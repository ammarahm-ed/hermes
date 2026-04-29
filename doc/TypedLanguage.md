# Typed Language

The typed language implementation is in progress and considered unstable. This is a usage guide that explains how to use it and what features currently work.

## Type System

The Flow type syntax is supported (use `-typed`).

The TypeScript type syntax is partially supported (use `-parse-ts` to convert TS to Flow before compilation).

### Primitive Types

* `void` (`undefined`)
* `null` (`null`)
* `boolean` (`true`, `false`)
* `string` (`"abc"`)
* `number` (`1`, `3.5`, `Infinity`, `NaN`)
* `bigint` (`1n`)

### Native Types

* `c_ptr` (raw C pointer)

### Any/Mixed

* `any` is a supertype of all types, casts implicitly to other types with a runtime type check.
* `mixed` is a supertype of all types, but does not cast implicitly.

### Empty

* `empty` has no values. It is the subtype of all other types.

### Unions

`A | B` indicates a union between two types `A` and `B`. Values that are either `A` or `B` are valid values of `A | B`. Unions may be cyclic, but cycles must include another type. For example: `type A = A` is invalid, but `type A = [A, B] | null` is valid.

### Arrays

The typed language implements Array as a generic: `Array<T>` or `T[]` both work, and mean the same thing.

These soundly typed arrays are incompatible with untyped JS arrays and are more restrictive. They do not allow holes or empty spaces: every element of an array `a` in the range `[0, a.length)` _must_ be of type `T`. Indexed access to arrays is bounds-checked and throws on out-of-bounds access.

### Tuple

Tuples are shown with brackets: `[A, B, C]`.

When instantiating a tuple, it is typically necessary to specify the type to avoid confusion with arrays:
```
var x: [number, string] = [1, "abc"];
```

### Objects

All object types in the typed language are _exact_ objects.
```
var obj: {a: number, b: string} = {a: 1, b: "xyz"};
```

They must have exactly the set of properties listed in their type (no extra properties allowed). Properties must be in the same order as the type requires. This allows the compiler to make access extremely efficient.

### Functions

Function types can be quite complex:
```
type F = (this: string, x: number, y?: boolean) => void;
```

The type can contain the type of `this` inside the function, as well as optional arguments.

If a function has NO type annotations, it is considered to be an "untyped" function, and calls to it _will not be typechecked_. This allows for convenient calling to untyped JS from typed JS without extra effort or modification.

### Classes

Classes are declared as in JS:
```
class C {
  x: number;
  y: number;

  constructor(x: number, y: number) {
    this.x = x;
    this.y = y;
  }
}
```

Classes in the typed language are _nominally typed_. This means that two classes that happen to have the same field names cannot be used interchangeably.

Class methods can be decorated as final, meaning they cannot be overridden by a subclass:
```
class A {
  @Hermes.final
  foo(): number { return 1; }
}
class B extends A {
  foo(): number { return 2; } // ERROR
}
```

### Optional Types

The `?` indicates that a type may also be either `null` or `void`:
```
type A = ?number;
```

### Type Aliases

Types can be aliased using the `type` keyword:
```
type A = number;
```

### Generics

Classes and type aliases can both be generic.

```
class Wrapper<T> { x: T }
type NumberTuple<T> = [T, number];
```

## Typechecking

### Compile-time Typechecking

The compiler will emit errors for any type errors which can be detected ahead of time.

### Runtime Typechecking

Certain operations will result in checked casts which must happen at runtime.

```
function foo(a: any) {
  var b: number = a; // Checked cast inserted, will throw if 'a' is not a number.
}
```

### Type Refinement

Typed language doesn't have full proper type refinement support yet but it supports a partial implementation that allows for certain use cases by implicitly checking `null` or `void` at usage time.

```
function foo(a: number | void): number {
  var b: number = a; // Typechecks at compile time, but will throw if 'a' is not a number.
  return b;
}
```

## JS Language Features

The typed language is a subset of JavaScript. Most common ES2015+ constructs work, with the notable exceptions listed under "Not yet supported" below.

### Function Parameters

Function parameters can be typed, optional, defaulted, or destructured.

```
// Typed parameter.
function f(x: number): void {}

// Optional parameter (must come last). Inside the body, type is `T | void`.
function g(x: number, y?: string): void {}

// Default parameter.
function h(x: number = 1): number { return x; }

// Object destructuring parameter, with optional default.
function i({x, y}: {x: number, y: string} = {x: 0, y: ""}): void {}

// Array (tuple) destructuring parameter, including nested patterns.
function j([a, b]: [number, string]): void {}
function k({a: {b}}: {a: {b: number}}): void {}
```

### Destructuring

Destructuring is supported in variable declarations and parameters, including nested patterns and default values. Type annotations inside destructuring patterns are not allowed; annotate the whole pattern instead.

```
const [x, y]: [number, string] = [1, "abc"];
const {a, b}: {a: number, b: number} = obj;
```

### Spread in Array Literals

Spread of an array into an array literal is supported.

```
const xs: number[] = [1, 2, ...other];
```

### Other Features

* Arrow functions (without type parameters).
* `for-of` loops over arrays.
* `try`/`catch`/`throw` (the catch binding is typed `any`).
* Template literals.
* Classes with static members, private fields and methods (`#name`),
  inheritance, `super` calls, and `super.method()` access.
* Getters and setters on classes (must be marked `@Hermes.final`).

### Not supported (yet)

* Spread arguments in calls (`f(...args)`).
* Rest elements in destructuring (`{a, ...rest}`, `[a, ...rest]`).
* Rest parameters in function declarations and function types
  (`function f(...args)`).
* Spread elements in tuple literals.
* `async` methods and generator methods.
* Computed property names in class definitions.
* Optional call expressions (`f?.()`).
* Type parameters on function expressions and arrow functions.
