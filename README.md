# Erkao

[![tests](https://github.com/Erwann9875/Erkao/actions/workflows/tests.yml/badge.svg)](https://github.com/Erwann9875/Erkao/actions/workflows/tests.yml)

Erkao is a small, fun, dynamic language written in C. It starts as an AST interpreter with classes, functions, arrays, and maps. The goal is to keep the core tiny but expressive so you can grow it into a full Windows-first language later.

## Build

```sh
cmake -S . -B build
cmake --build build
```

On Windows, you can use the setup script:

```powershell
.\scripts\erkao.ps1
```

If you install Visual Studio Build Tools or Visual Studio, make sure **Desktop development with C++** is selected.
You can also specify a generator: `.\scripts\erkao.ps1 build -Generator "Ninja"`.

## CLI

Help and version:

```sh
./build/Debug/erkao.exe --help
./build/Debug/erkao.exe --version
```

Run a file:

```sh
./build/Debug/erkao.exe run ./examples/hello.ek
```

Start a REPL:

```sh
./build/Debug/erkao.exe repl
```

Start a REPL (no args):

```sh
./build/Debug/erkao.exe
```

Backward-compatible shorthand:

```sh
./build/Debug/erkao.exe ./examples/hello.ek
```

## Testing

Run the golden tests from the repo root:

```powershell
.\scripts\run-tests.ps1
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
- Imports: `import "path/to/file.ek" as name;`
- Functions: `fun name(a, b) { return a + b; }`
- Classes: `class Name { fun init(...) { ... } fun method(...) { ... } }`
- Arrays: `[1, 2, 3]`, indexing with `arr[0]`
- Maps: `{ key: value, "other": value }`, indexing with `map["key"]`

## Imports

Use `import` to load and execute another `.ek` file once.

```ek
import "./examples/hello.ek" as hello;
```

Semantics:

- The import path must be a string.
- Relative paths resolve from the importing file's directory.
- If the path has no extension, `.ek` is appended.
- Each file is loaded and executed at most once.
- `as name` binds the module object; use `name.symbol` to access exports.

## Keywords

`let`, `fun`, `class`, `if`, `else`, `while`, `import`, `return`, `true`, `false`, `null`, `this`, `and`, `or`

## Built-in functions

- `print(...)`
- `clock()`
- `type(value)`
- `len(value)`
- `args()`
- `push(array, value)`
- `keys(map)`
- `values(map)`

## Stdlib modules

- `fs.exists(path)`
- `fs.readText(path)`
- `fs.writeText(path, text)`
- `fs.listDir(path)`
- `fs.cwd()`
- `path.join(left, right)`
- `path.dirname(path)`
- `path.basename(path)`
- `path.extname(path)`
- `proc.run(cmd)`
- `time.now()`
- `time.sleep(seconds)`
- `env.get(name)`
- `env.args()`
- `plugin.load(path)`

## Plugins

Plugins are DLLs that export `erkao_init` and register native functions.

```c
#include "erkao_plugin.h"

static Value hello(VM* vm, int argc, Value* args) {
  (void)vm;
  (void)argc;
  (void)args;
  printf("hello from plugin\n");
  return NULL_VAL;
}

bool erkao_init(ErkaoApi* api) {
  if (api->apiVersion != ERKAO_PLUGIN_API_VERSION) return false;
  api->defineNative(api->vm, "pluginHello", hello, 0);
  return true;
}
```

Build a plugin with include paths to `include` and `src`.

## Notes

- Strings are UTF-8 byte sequences (no unicode processing yet).
- Mark-and-sweep GC runs at statement boundaries to reclaim runtime objects.
- Source and AST programs are freed when no live functions reference them.
