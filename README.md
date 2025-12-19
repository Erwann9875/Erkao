# Erkao

Erkao is a small, fun, dynamic language written in C. It starts as an AST interpreter with classes, functions, arrays, and maps. The goal is to keep the core tiny but expressive so you can grow it into a full Windows-first language later.

## Build

```sh
cmake -S . -B build
cmake --build build
```

Run a file:

```sh
build/erkao examples/hello.ek
```

Start a REPL (no args):

```sh
build/erkao
```

## Language quick tour

```ek
let name = "Erkao";

fun greet(who) {
  return "Hey " + who + "!";
}

class Wizard {
  fun init(name) {
    this.name = name;
  }

  fun cast(spell) {
    print(this.name + " casts " + spell);
  }
}

let wiz = Wizard("Ada");
wiz.cast("light");

let nums = [1, 2, 3];
nums[3] = 4;
print(len(nums));

let config = { host: "localhost", port: "8080" };
print(config["host"]);
print(greet(name));
```

## Syntax overview

- Variables: `let x = 3;`
- Control flow: `if (...) { ... } else { ... }`, `while (...) { ... }`
- Functions: `fun name(a, b) { return a + b; }`
- Classes: `class Name { fun init(...) { ... } fun method(...) { ... } }`
- Arrays: `[1, 2, 3]`, indexing with `arr[0]`
- Maps: `{ key: value, "other": value }`, indexing with `map["key"]`

## Keywords

`let`, `fun`, `class`, `if`, `else`, `while`, `return`, `true`, `false`, `null`, `this`, `and`, `or`

## Built-in functions

- `print(...)`
- `clock()`
- `type(value)`
- `len(value)`
- `args()`

## Notes

- Strings are UTF-8 byte sequences (no unicode processing yet).
- Memory is freed at process exit (no GC yet).
