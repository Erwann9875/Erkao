# ADR-0001: Layered Architecture Contract

- Status: Accepted
- Date: 2026-02-05

## Context

Erkao has grown quickly and now contains several large modules with cross-layer
coupling. This makes refactors risky and slows feature work.

We need a stable architecture contract for:

- Compiler pipeline boundaries
- Runtime/module boundaries
- Dependency direction rules
- Controlled migration from current state to target state

## Decision

We adopt a strict layered contract for core language architecture:

1. `frontend`
2. `sema`
3. `backend`
4. `runtime`
5. `stdlib`

Supporting layers:

- `foundation` (shared low-level utilities)
- `app` (CLI/tooling entrypoints)
- `external` (public headers in `include/`)

### Allowed dependency direction

- `foundation` -> `foundation`, `external`
- `frontend` -> `foundation`, `frontend`, `external`
- `sema` -> `foundation`, `frontend`, `sema`, `external`
- `backend` -> `foundation`, `frontend`, `sema`, `backend`, `external`
- `runtime` -> `foundation`, `backend`, `runtime`, `external`
- `stdlib` -> `foundation`, `runtime`, `stdlib`, `external`
- `app` -> any

Lower-level layers must not include higher-level layers.

### Module-to-layer mapping

Defined in `docs/architecture_contract.json`.

### Public API rules

- Each layer exposes a minimal public header surface.
- Internal-only headers remain private to their layer.
- New modules must declare their layer in the contract file.

### Enforcement

- CI runs `python ./scripts/check-architecture.py`.
- Existing violations are frozen in `docs/architecture_violations.json`.
- New violations fail CI.

## Consequences

- We can refactor incrementally without destabilizing behavior.
- Architectural debt becomes explicit and measurable.
- Large refactors are decomposed into layer-by-layer migrations.

## Migration Plan

1. Freeze current violations (done in this ADR).
2. Remove violations by priority:
   - `frontend -> backend/runtime`
   - `sema -> backend`
   - other cross-layer includes
3. Delete entries from `docs/architecture_violations.json` as each violation is resolved.
