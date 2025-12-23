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
./scripts/erkao.ps1
```

If you install Visual Studio Build Tools or Visual Studio, make sure **Desktop development with C++** is selected.
You can also specify a generator: `./scripts/erkao.ps1 build -Generator "Ninja"`.

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

Run with bytecode output:

```sh
./build/Debug/erkao.exe run --bytecode ./examples/hello.ek
```

Package manager:

```sh
./build/Debug/erkao.exe pkg init
./build/Debug/erkao.exe pkg add ../path/to/pkg
./build/Debug/erkao.exe pkg add ../path/to/pkg --global
./build/Debug/erkao.exe pkg install
./build/Debug/erkao.exe pkg list
```

On macOS/Linux, use `./build/erkao` instead of `./build/Debug/erkao.exe`.

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

Format a file:

```sh
./build/Debug/erkao.exe fmt ./examples/hello.ek
```

Check formatting only:

```sh
./build/Debug/erkao.exe fmt --check ./examples/hello.ek
```

Lint a file:

```sh
./build/Debug/erkao.exe lint ./examples/hello.ek
```

## Testing

Run the golden tests from the repo root:

```powershell
./scripts/run-tests.ps1
```

Run the GC stress test with logging:

```powershell
./scripts/run-gc-stress.ps1
```

Format tests and examples:

```powershell
./scripts/format.ps1
```

Lint tests and examples:

```powershell
./scripts/lint.ps1
```

Lint checks: whitespace/indentation, long lines, unused locals/assignments,
unreachable code, and break/continue misuse.

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

## Packaging

Erkao supports versioned packages stored under `packages/<name>/<version>/`.

Example manifest (`erkao.mod`):

```
module my-app 0.1.0
require alpha 1.0.0
```

Lockfile (`erkao.lock`):

```
lock 1
alpha 1.0.0
```

Import resolution order for non-relative paths:

1. `ERKAO_PATH` entries and `--module-path` entries.
2. `packages/<name>/<version>` (locked if present, otherwise latest installed).
3. Global cache at `%USERPROFILE%\.erkao\packages` or `~/.erkao/packages`
   (override with `ERKAO_PACKAGES`).

Package entry points:

- `packages/<name>/<version>/main.ek`
- `packages/<name>/<version>/index.ek`

`pkg add` expects a folder that contains `erkao.mod` and will copy it into
`packages/<name>/<version>/`. Use `--global` to also cache it in the global store.

Versioned imports:

```
import "alpha@1.0.0" as alpha;
import "alpha/utils" as utils;
```

## Keywords

`let`, `fun`, `class`, `if`, `else`, `while`, `import`, `as`, `return`, `true`, `false`, `null`, `this`, `and`, `or`

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
- `json.parse(text)`
- `json.stringify(value)`
- `math.abs(x)`
- `math.floor(x)`
- `math.ceil(x)`
- `math.round(x)`
- `math.sqrt(x)`
- `math.pow(x, y)`
- `math.min(...)`
- `math.max(...)`
- `math.clamp(value, min, max)`
- `math.PI`
- `math.E`
- `http.get(url)`
- `http.post(url, body)`
- `http.request(method, url, body)`
- `http.serve(port, routes)`
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
- Set `ERKAO_GC_LOG=1` to print GC stats to stderr.
- REPL history is stored in `~/.erkao_history` or `%USERPROFILE%\.erkao_history` (override with `ERKAO_HISTORY`).
- Source and AST programs are freed when no live functions reference them.
- `http` uses WinHTTP on Windows and libcurl on macOS/Linux (libcurl headers required at build time).
- `http` responses are maps with `status`, `body`, and `headers`.
- `http.serve` binds to `127.0.0.1`; pass `0`/`null` for a random port. If a port is in use, it picks a free one and prints the chosen port.
- `http.serve` routes map keys can be `"/path"` (any method) or `"GET /path"` (specific method); values can be:
  - A body string (returned with status 200)
  - A map with `status`, `body`, and `headers`
  - A function that receives a request object `{ method, path, headers, body }` and returns a string or response map
- `http.serve` logs requests as `[IP] [YYYY-MM-DD HH:MM:SS] Called /path`.
- HTTP tests run by default and use the built-in HTTP server; set `ERKAO_HTTP_TEST=0` to skip.
- `ERKAO_PATH` adds module search paths (separated by `;` on Windows, `:` elsewhere).
- `ERKAO_PACKAGES` overrides the global packages directory.
