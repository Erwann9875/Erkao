# Ownership Conventions

Erkao C code follows explicit ownership naming:

- `new*` / `create*`: returns an owned value. Caller must release or transfer ownership.
- `take*`: consumes caller-owned input and assumes ownership.
- `copy*` / `clone*`: returns a new owned copy.
- `borrow*` / raw pointer accessors: non-owning view valid only while parent object lives.

## Rules

1. Every API that allocates must document ownership in the header comment.
2. Borrowed pointers must never be freed or stored past owner lifetime.
3. Transfer points must be single and explicit (no implicit double-ownership).
4. Library/runtime code must report errors through runtime diagnostics, never `exit(1)`.
5. Allocation/copy code paths must check OOM and propagate failure.

## Review Expectations

During review, verify:

- allocation path and deallocation path are paired
- ownership transfer is explicit at call sites
- error cleanup releases partially-owned resources
- APIs preserve `new/take/borrow` semantics in names and behavior
