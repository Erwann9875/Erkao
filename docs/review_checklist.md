# Code Review Checklist

Mandatory checklist for non-trivial changes:

- API contract: behavior, inputs, outputs, and compatibility are explicit.
- Error paths: invalid input and OOM paths are handled and tested.
- Memory ownership: `new/take/borrow` semantics are preserved.
- Tests: new behavior and regressions are covered.
- Security impact: injection/path traversal/policy bypass risks considered.
- Performance impact: hot-path costs measured or justified.

Critical modules should include a short design note under `docs/design-notes/`.
