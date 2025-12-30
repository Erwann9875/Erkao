# Database and ORM

This page shows the built-in `db` module and the `orm` package.
All examples assume you run scripts with `erkao run <file>` and end statements
with semicolons.

## Quick start (memory driver)

```ek
let conn = db.connect("memory://");
print(conn.driver); // memory
print(conn.kind);   // document

db.insert(conn, "users", { name: "Ada", age: 42 });
db.insert(conn, "users", { name: "Bob", age: 30 });

let rows = db.find(conn, "users", { age: 42 });
print(len(rows));

let updated = db.update(conn, "users", { name: "Ada" }, { age: 43 });
print(updated);

let removed = db.delete(conn, "users", { name: "Bob" });
print(removed);

db.close(conn);
```

Memory driver options:

- `db.find(conn, "users", query, { limit: 10, skip: 5 })`
- `db.update(conn, "users", query, update, { multi: false })`
- `db.delete(conn, "users", query, { multi: false })`

## SQL drivers (Postgres/MySQL)

The SQL helpers build simple queries from maps. Keys become column names and
values become parameters.

```ek
let conn = db.connect("postgres://user:pass@localhost:5432/app");
let rows = db.find(conn, "users", { status: "active" }, { limit: 10, offset: 0 });
let inserted = db.insert(conn, "users", { name: "Ada", age: 42 });
let changed = db.update(conn, "users", { name: "Ada" }, { age: 43 });
let removed = db.delete(conn, "users", { name: "Ada" });
```

For custom SQL, use `db.exec`:

```ek
// Postgres uses $1, $2, ...
let res = db.exec(conn, "SELECT id, name FROM users WHERE age > $1", [30]);
print(res.rows);
```

MySQL uses `?` placeholders:

```ek
let conn = db.connect("mysql://user:pass@localhost:3306/app");
let res = db.exec(conn, "SELECT id, name FROM users WHERE age > ?", [30]);
```

Notes:

- Only simple equality is supported by `db.find`/`db.update`/`db.delete`.
- SQL identifiers must be alphanumeric plus `_` or `.`.

## MongoDB

Mongo uses map/array values and simple equality filters (no operators yet).

```ek
let conn = db.connect("mongo://localhost:27017/app");
db.insert(conn, "users", { name: "Ada", age: 42 });
let rows = db.find(conn, "users", { age: 42 });
```

## ORM package

The `orm` package is a thin layer over `db`.

```ek
import "orm" as orm;

let conn = db.connect("memory://");
let User = orm.model(conn, "users");

User.insert({ name: "Ada", age: 42 });
let rows = User.find({ age: 42 });
let one = User.findOne({ name: "Ada" });
User.update({ name: "Ada" }, { age: 43 });
User.remove({ name: "Ada" });
```

## Migrations

Define schemas on your models and apply them with `orm.migrate`.

```ek
import "orm" as orm;

let conn = db.connect("memory://");
let User = orm.model(conn, "users", { name: "string", age: "number" });
let Task = orm.model(conn, "tasks", { title: "string", done: "bool" }, { version: 2 });

let applied = orm.migrate([User, Task]);
print("applied", applied);
```

Notes:

- SQL drivers create tables with `CREATE TABLE IF NOT EXISTS`.
- For Postgres/MySQL, missing columns are added when the schema grows.
- Document drivers only record migrations; they do not enforce schemas.
- Bump `version` (number) to apply a new migration name for the same model.

Supported schema type names:

- `string`/`text` -> `TEXT`
- `number`/`float`/`double` -> `DOUBLE`
- `int`/`integer` -> `INTEGER`
- `bool`/`boolean` -> `BOOLEAN`
- `json` -> `JSONB` (Postgres) or `JSON` (MySQL)
- `datetime`/`timestamp` -> `TIMESTAMP`

## Driver availability

```ek
print(db.drivers());        // known driver names
print(db.supports("mongo")); // true if the name is registered
```

`db.supports` reports whether a name is recognized. `db.connect` may still fail
if the native client library is not available.

To enable native drivers, install the client libraries and configure:

```sh
cmake -S . -B build -DERKAO_DB_POSTGRES=ON -DERKAO_DB_MYSQL=ON -DERKAO_DB_MONGO=ON
```
