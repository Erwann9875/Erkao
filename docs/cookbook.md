# Erkao Cookbook

Practical, copy-pasteable recipes plus two guided builds (CLI app + mini game).
All examples assume you run scripts with `erkao run <file>` and end statements
with semicolons.

## Quick recipes

### Read/write a JSON config

```ek
import json from "json";
import fs from "fs";

let path = "./config.json";

if (!fs.exists(path)) {
  let initial = { name: "Player", volume: 0.8, fullscreen: false };
  fs.writeText(path, json.stringify(initial));
}

let text = fs.readText(path);
let cfg = json.parse(text);
print("Hello " + cfg["name"]);
```

### Use command line args

```ek
let argv = args();
if (len(argv) < 2) {
  print("Usage: app.ek <name>");
} else {
  print("Hi " + argv[1]);
}
```

### HTTP GET (fetch JSON)

```ek
import http from "http";
import json from "json";

let res = http.get("https://httpbin.org/json");
let data = json.parse(res["body"]);
print(data["slideshow"]["title"]);
```

### Simple file copy

```ek
import fs from "fs";

let src = "./input.txt";
let dst = "./output.txt";
fs.writeText(dst, fs.readText(src));
```

### Safe map access

```ek
let user = { name: "Erkao" };
let city = user?.city;
print(city); // null
```

### Array pipelines

```ek
import array from "array";

let nums = [1, 2, 3, 4];
fun double(n) {
  return n * 2;
}
fun isEven(n) {
  return n % 2 == 0;
}
let doubled = array.map(nums, double);
let even = array.filter(doubled, isEven);
print(even);
```

## Guide: build a tiny CLI todo app

Goal: a simple `todo.ek` that stores tasks in a JSON file.

### 1) Data helpers

```ek
import fs from "fs";
import json from "json";

let store = "./todo.json";

fun loadTodos() {
  if (!fs.exists(store)) return [];
  let text = fs.readText(store);
  let items = json.parse(text);
  return items;
}

fun saveTodos(items) {
  fs.writeText(store, json.stringify(items));
}
```

### 2) Commands

```ek
fun addTodo(text) {
  let items = loadTodos();
  let item = { text: text, done: false };
  push(items, item);
  saveTodos(items);
  print("Added.");
}

fun listTodos() {
  let items = loadTodos();
  let i = 0;
  while (i < len(items)) {
    let item = items[i];
    let mark = item["done"] ? "[x]" : "[ ]";
    print(mark + " " + i + ": " + item["text"]);
    i = i + 1;
  }
}

fun doneTodo(index) {
  let items = loadTodos();
  if (index < 0 || index >= len(items)) {
    print("Invalid index.");
    return;
  }
  items[index]["done"] = true;
  saveTodos(items);
  print("Done.");
}
```

### 3) Wire the CLI

```ek
let argv = args();
if (len(argv) < 2) {
  print("Usage: todo.ek add <text> | list | done <index>");
} else {
  let cmd = argv[1];
  if (cmd == "add") {
    if (len(argv) < 3) {
      print("Missing text.");
    } else {
      addTodo(argv[2]);
    }
  } else if (cmd == "list") {
    listTodos();
  } else if (cmd == "done") {
    if (len(argv) < 3) {
      print("Missing index.");
    } else {
      let index = json.parse(argv[2]);
      doneTodo(index);
    }
  } else {
    print("Unknown command.");
  }
}
```

Run:

```sh
./build/Debug/erkao.exe run ./examples/todo.ek add "Ship the game"
./build/Debug/erkao.exe run ./examples/todo.ek list
```

## Guide: build a tiny arcade game (Dodger)

Goal: a small game with a player square dodging falling blocks.
Requires SDL2 graphics (the `gfx` module).

### 1) Setup + game loop

```ek
import gfx from "gfx";
import random from "random";
import array from "array";

let width = 800;
let height = 480;
gfx.init("Erkao Dodger", width, height);

let player = { x: 80, y: 360, w: 36, h: 36, speed: 280 };
let obstacles = [];
let spawnTimer = 0;
let score = 0;

fun spawnObstacle() {
  let w = 30 + random.int(60);
  let x = width + w;
  let y = 60 + random.int(height - 120);
  let o = { x: x, y: y, w: w, h: 20, speed: 180 + random.int(120) };
  push(obstacles, o);
}

fun overlap(a, b) {
  return a["x"] < b["x"] + b["w"] &&
         a["x"] + a["w"] > b["x"] &&
         a["y"] < b["y"] + b["h"] &&
         a["y"] + a["h"] > b["y"];
}

let last = clock();
while (gfx.poll()) {
  let now = clock();
  let dt = now - last;
  last = now;

  // Input
  let moveY = 0;
  if (gfx.key("up") || gfx.key("w")) moveY = moveY - 1;
  if (gfx.key("down") || gfx.key("s")) moveY = moveY + 1;
  player["y"] = player["y"] + moveY * player["speed"] * dt;
  if (player["y"] < 40) player["y"] = 40;
  if (player["y"] > height - 40 - player["h"]) player["y"] = height - 40 - player["h"];

  // Spawn
  spawnTimer = spawnTimer + dt;
  if (spawnTimer > 0.8) {
    spawnTimer = 0;
    spawnObstacle();
  }

  // Update
  let i = 0;
  while (i < len(obstacles)) {
    obstacles[i]["x"] = obstacles[i]["x"] - obstacles[i]["speed"] * dt;
    if (obstacles[i]["x"] < -100) {
      // remove by swap
      obstacles[i] = obstacles[len(obstacles) - 1];
      obstacles[len(obstacles) - 1] = null;
      obstacles = array.slice(obstacles, 0, len(obstacles) - 1);
      score = score + 1;
    } else {
      i = i + 1;
    }
  }

  // Collision
  let hit = false;
  i = 0;
  while (i < len(obstacles)) {
    if (overlap(player, obstacles[i])) {
      hit = true;
      break;
    }
    i = i + 1;
  }

  // Draw
  gfx.clear("skyblue");
  gfx.rect("lightgray", 0, height - 60, width, 60);
  gfx.rect("navy", player["x"], player["y"], player["w"], player["h"]);
  i = 0;
  while (i < len(obstacles)) {
    let o = obstacles[i];
    gfx.rect("tomato", o["x"], o["y"], o["w"], o["h"]);
    i = i + 1;
  }
  gfx.text("Score " + toText(score), 20, 20, "white", 18);

  if (hit) {
    gfx.text("Game Over", 320, 200, "white", 32);
    gfx.text("Press R to restart", 290, 240, "white", 16);
    if (gfx.keyPressed("r")) {
      obstacles = [];
      score = 0;
    }
  }

  gfx.present();
}

gfx.quit();
```

### 2) Make it nicer

Ideas:
- Add parallax background rectangles that scroll at different speeds.
- Use `gfx.sprite` for player and obstacles.
- Add a start screen and restart animation.
- Add sound effects when available.

## Patterns and tips

- Break big scripts into helpers at the top (pure functions for logic).
- Prefer maps for structured data: `{ x: 0, y: 0 }`.
- Use `array.slice` and `array.filter` to manage collections.
- For performance, avoid heavy string concatenation in tight loops.
