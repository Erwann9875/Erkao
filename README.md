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

### Windows graphics builds (SDL2)

SDL2 binaries must match the compiler toolchain.

MSYS2 (UCRT64) build:

```sh
cmake --preset msys2-debug
cmake --build --preset msys2-debug
```

MSVC build (with vcpkg):

```sh
cmake --preset msvc-debug -DCMAKE_TOOLCHAIN_FILE=C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build --preset msvc-debug
```

You can also use the PowerShell helper:

```powershell
./scripts/erkao.ps1 build -Preset msys2-debug
./scripts/erkao.ps1 build -Preset msvc-debug
```

For MSVC builds, the script will bootstrap vcpkg and install SDL2 if needed.
Set `VCPKG_ROOT` or pass `-VcpkgRoot` to reuse an existing vcpkg install.
If you already configured a preset without vcpkg, rerun with `-Clean` to reset the build dir.

If `gfx` is undefined at runtime, graphics were disabled because SDL2 was not found for that toolchain.

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

## VS Code extension (syntax + LSP)

This repo includes a VS Code extension for `.ek` syntax highlighting plus
autocomplete/hover/go-to-definition at `vscode/erkao-syntax`.

Run it in development mode:

```powershell
# From the repo root
cd vscode/erkao-syntax
npm install
code --extensionDevelopmentPath="$PWD"
```

Or open `vscode/erkao-syntax` in VS Code and press `F5` to launch the Extension
Development Host, then open any `.ek` file.

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

Tooling config (optional):

Create `erkao.tooling` in the repo root (or pass `--config path`):

```
# erkao.tooling
format.ruleset = standard
format.indent = 2
lint.ruleset = strict
lint.maxLine = 100
lint.rules = trailing,tabs,indent,line-length,flow,lex
```

Rulesets:
- Format: `standard` (2 spaces), `wide` (4 spaces)
- Lint: `basic`, `default`, `strict`

Overrides:
- `fmt --ruleset NAME --indent N`
- `lint --ruleset NAME --max-line N --rules list`

## Testing

Run the snapshot (golden) tests from the repo root:

```powershell
./scripts/run-tests.ps1
```

```sh
./scripts/run-tests.sh
```

OS-specific wrappers are also available:

```powershell
./scripts/windows/run-tests.ps1
```

```sh
./scripts/unix/run-tests.sh
```

Update snapshots (golden outputs):

```powershell
./scripts/run-tests.ps1 -Update
./scripts/update-tests.ps1
```

```sh
./scripts/run-tests.sh --update
./scripts/update-tests.sh
```

Save actual output files for diffs (`.out.actual`):

```powershell
./scripts/run-tests.ps1 -WriteActual
```

```sh
./scripts/run-tests.sh --write-actual
```

Run the GC stress test with logging:

```powershell
./scripts/run-gc-stress.ps1
```

```sh
./scripts/run-gc-stress.sh
```

```sh
./scripts/unix/run-gc-stress.sh
```

Format tests and examples:

```powershell
./scripts/format.ps1
```

```sh
./scripts/format.sh
```

```sh
./scripts/unix/format.sh
```

Lint tests and examples:

```powershell
./scripts/lint.ps1
```

```sh
./scripts/lint.sh
```

```sh
./scripts/unix/lint.sh
```

Lint checks: trailing whitespace, tabs, indentation, long lines, and (when enabled)
flow/lex errors.

## Benchmarks

Run the microbenchmarks from the repo root:

```powershell
./scripts/run-bench.ps1
./scripts/run-bench.ps1 -Repeat 3
```

```sh
./scripts/run-bench.sh
./scripts/run-bench.sh --repeat 3
```

```sh
./scripts/unix/run-bench.sh --repeat 3
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
- Constants: `const x = 3;`
- Control flow: `if (...) { ... } else { ... }`, `while (...) { ... }`
- Match: `match (value) { case 1: ... }` (alias of `switch`)
- Imports: `import "path/to/file.ek" as name;`, `import name from "path";`, `import * as name from "path";`
- Functions: `fun name(a, b) { return a + b; }`
- Classes: `class Name { fun init(...) { ... } fun method(...) { ... } }`
- Arrays: `[1, 2, 3]`, indexing with `arr[0]`
- Maps: `{ key: value, "other": value }`, indexing with `map["key"]`
- Enums: `enum Color { Red, Green, Blue }`, access with `Color["Red"]`
- Strings: `"Hello ${name}"` and multiline `"""line 1\nline 2"""`
- Optional chaining: `user?.profile` returns `null` if `user` is `null`

## Imports

Use `import` to load and execute another `.ek` file once.

```ek
import "./examples/hello.ek" as hello;
import greet from "./examples/hello.ek";
import * as helloAll from "./examples/hello.ek";
```

Semantics:

- The import path must be a string.
- Relative paths resolve from the importing file's directory.
- If the path has no extension, `.ek` is appended.
- Each file is loaded and executed at most once.
- `import * as name from` (or legacy `import "path" as name`) binds the module object.
- `import name from` reads the module's `default` export.
- If a module contains any `export` statements, only exported names are exposed.
  Otherwise all top-level bindings are exposed (backward compatible).

Exports (top-level only):

```ek
export let answer = 42;
export fun greet(name) { return "Hi " + name; }
export default "Hello";
export { answer as value };
export * from "./other.ek";
export { default as other } from "./other.ek";
```

## Packaging

Erkao supports versioned packages stored under `packages/<name>/<version>/`.

Example manifest (`erkao.mod`) with ranges:

```
module my-app 0.1.0
require alpha ^1.0.0
require beta 0.5.0
```

Optional package entry points and exports:

```
main main.ek
export utils utils.ek
```

`main` sets the default file for `import "alpha"`. `export` maps package subpaths
like `import "alpha/utils"` to a file.

Lockfile (`erkao.lock`), generated by `pkg install`:

```
lock 1
alpha 1.0.0
beta 0.5.0
```

Supported ranges: exact `1.2.3`, caret `^1.2.3`, tilde `~1.2`, wildcards `1.*`,
and comparisons like `>=1.0.0 <2.0.0`. `pkg install` resolves ranges to the
highest available version in `packages/` or the global cache, then writes exact
versions into `erkao.lock`.

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

Versioned imports (ranges allowed):

```
import "alpha@^1.0.0" as alpha;
import "alpha/utils" as utils;
```

See `examples/packages_semver.ek` for a full example.

## Keywords

`and`, `as`, `break`, `case`, `class`, `const`, `continue`, `default`, `else`, `enum`, `export`, `false`, `for`, `foreach`, `from`, `fun`, `if`, `import`, `in`, `let`, `match`, `null`, `or`, `return`, `switch`, `this`, `true`, `while`

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
- `fs.isFile(path)`
- `fs.isDir(path)`
- `fs.size(path)`
- `fs.glob(pattern)`
- `path.join(left, right)`
- `path.dirname(path)`
- `path.basename(path)`
- `path.extname(path)`
- `path.isAbs(path)`
- `path.normalize(path)`
- `path.stem(path)`
- `path.split(path)`
- `json.parse(text)`
- `json.stringify(value)`
- `yaml.parse(text)`
- `yaml.stringify(value)`
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
- `random.seed(value)`
- `random.int(max)` / `random.int(min, max)`
- `random.float()` / `random.float(min, max)`
- `random.choice(array)`
- `random.normal(mean, stddev)`
- `random.gaussian(mean, stddev)`
- `random.exponential(lambda)`
- `random.uniform()` / `random.uniform(min, max)`
- `str.upper(text)`
- `str.lower(text)`
- `str.trim(text)`
- `str.trimStart(text)`
- `str.trimEnd(text)`
- `str.startsWith(text, prefix)`
- `str.endsWith(text, suffix)`
- `str.contains(text, needle)`
- `str.split(text, sep)`
- `str.join(array, sep)`
- `str.builder()`
- `str.append(builder, text)`
- `str.build(builder, sep?)`
- `str.replace(text, needle, replacement)`
- `str.replaceAll(text, needle, replacement)`
- `str.repeat(text, count)`
- `array.slice(array, start?, end?)`
- `array.map(array, fn)`
- `array.filter(array, fn)`
- `array.reduce(array, fn, initial?)`
- `array.contains(array, value)`
- `array.indexOf(array, value)`
- `array.concat(left, right)`
- `array.reverse(array)`
- `os.platform()`
- `os.arch()`
- `os.sep()`
- `os.eol()`
- `os.cwd()`
- `os.home()`
- `os.tmp()`
- `http.get(url)`
- `http.post(url, body)`
- `http.request(method, url, body)`
- `http.serve(port, routes)`
- `proc.run(cmd)`
- `time.now()`
- `time.sleep(seconds)`
- `time.format(timestamp, format, utc?)`
- `time.iso(timestamp, utc?)`
- `time.parts(timestamp, utc?)`
- `env.get(name)`
- `env.set(name, value)`
- `env.has(name)`
- `env.unset(name)`
- `env.all()`
- `env.args()`
- `plugin.load(path)`
- `vec2.make(x, y)` / `vec3.make(x, y, z)` / `vec4.make(x, y, z, w)`
- `vec2.add(a, b)` / `vec3.add(a, b)` / `vec4.add(a, b)`
- `vec2.sub(a, b)` / `vec3.sub(a, b)` / `vec4.sub(a, b)`
- `vec2.scale(v, s)` / `vec3.scale(v, s)` / `vec4.scale(v, s)`
- `vec2.dot(a, b)` / `vec3.dot(a, b)` / `vec4.dot(a, b)`
- `vec2.len(v)` / `vec3.len(v)` / `vec4.len(v)`
- `vec2.norm(v)` / `vec3.norm(v)` / `vec4.norm(v)`
- `vec2.lerp(a, b, t)` / `vec3.lerp(a, b, t)` / `vec4.lerp(a, b, t)`
- `vec2.dist(a, b)` / `vec3.dist(a, b)` / `vec4.dist(a, b)`
- `vec3.cross(a, b)`

## Graphics (gfx)

The SDL2 graphics module is available when built with SDL2 support.

Camera + sprites:
- `gfx.camera(x, y)` / `gfx.camera()` returns `{x, y}`
- `gfx.cameraZoom(z)` / `gfx.cameraZoom()`
- `gfx.worldToScreen(x, y)` and `gfx.screenToWorld(x, y)`
- `gfx.mouseWorld()` uses the current camera
- `gfx.sprite(path, frameW?, frameH?)`
- `gfx.spriteDraw(sprite, x, y, frame?, scale?, angle?, flip?)` (`flip`: `"x"`, `"y"`, `"xy"`)
Camera offset/zoom applies to draw calls (`rect`, `circle`, `line`, `image`, `text`, etc).

Input helpers:
- `gfx.key(name)` and `gfx.keyPressed(name)`
- `gfx.poll()` returns false on quit
- `gfx.pollEvent()` returns a map or `null` if no events are pending
- `gfx.textInput(enable?)` enables/disables text input (text events are `type: "text"`, default is off)

Event maps include:
- `type`: `"keyDown"`, `"keyUp"`, `"text"`, or `"quit"`
- `key`: optional string name (when known)
- `scancode`: numeric SDL scancode for key events
- `repeat`: boolean for keyDown events
- `text`: text payload for text events

## Plugins

Plugins are shared libraries that export `erkao_init` and register native functions.
The plugin ABI is versioned and extensible through `ErkaoApi.size` + feature flags.

```c
#include "erkao_plugin.h"

static Value hello(VM* vm, int argc, Value* args) {
  (void)vm;
  (void)argc;
  (void)args;
  printf("hello from plugin\n");
  return NULL_VAL;
}

static Value add(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!IS_NUMBER(args[0]) || !IS_NUMBER(args[1])) return NULL_VAL;
  return NUMBER_VAL(AS_NUMBER(args[0]) + AS_NUMBER(args[1]));
}

ERKAO_PLUGIN_EXPORT bool erkao_init(ErkaoApi* api) {
  if (api->apiVersion < ERKAO_PLUGIN_API_VERSION) return false;
  if ((api->features & ERKAO_PLUGIN_FEATURE_MODULES) != 0 && api->createModule) {
    ErkaoModule* module = api->createModule(api->vm, "hello");
    api->moduleAddNative(api->vm, module, "say", hello, 0);
    api->moduleAddNative(api->vm, module, "add", add, 2);
    api->defineModule(api->vm, "hello", module);
  } else {
    api->defineNative(api->vm, "helloSay", hello, 0);
    api->defineNative(api->vm, "helloAdd", add, 2);
  }
  return true;
}
```

Build a plugin with include paths to `include` and `src`:

```sh
# Windows (MSVC)
cl /LD /I include /I src examples/plugins/hello_plugin.c /Fe:hello_plugin.dll

# macOS
clang -shared -fPIC -I include -I src examples/plugins/hello_plugin.c -o hello_plugin.dylib

# Linux
gcc -shared -fPIC -I include -I src examples/plugins/hello_plugin.c -o hello_plugin.so
```

Use it from Erkao (extension depends on platform):

```ek
plugin.load("./examples/plugins/hello_plugin.dll");
print(hello.add(2, 3));
```

If you prefer globals, the fallback in the example registers `helloSay` and `helloAdd`.
See `examples/plugins/hello_plugin.c` and `examples/plugins/hello_plugin.ek`.

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
- `http.serve(port, routes, cors)` accepts an optional third parameter for CORS config: `{ origin: "*", methods: "GET, POST", headers: "Content-Type" }`
- `http.serve` logs requests as `[IP] [YYYY-MM-DD HH:MM:SS] Called /path`.
- HTTP tests run by default and use the built-in HTTP server; set `ERKAO_HTTP_TEST=0` to skip.
- `ERKAO_PATH` adds module search paths (separated by `;` on Windows, `:` elsewhere).
- `ERKAO_PACKAGES` overrides the global packages directory.
