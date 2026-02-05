# Compiler pipeline

Erkao routes `compile()` through explicit stage contracts:

1. Frontend stage (`src/frontend/pipeline_frontend.c`)
2. Sema stage (`src/typecheck/pipeline_sema.c`)
3. Lower/codegen stage (`src/bytecode/pipeline_lower.c`)
4. Pipeline orchestration (`src/bytecode/compiler_pipeline.c`)

## Stage contracts

Frontend now builds a structured `FrontendUnit` from tokens/source:

- validates stream invariants (`TOKEN_ERROR` rejection, final `TOKEN_EOF`)
- captures feature flags (imports, exports, classes, loops, match/switch, etc.)
- builds a top-level AST node list (`FrontendAst`) with token spans
- derives top-level declaration stats from AST nodes
- captures depth stats (paren/brace/bracket/interpolation max depth)

Sema builds a `SemaUnit` summary from `FrontendUnit`:

- module kind (`script` vs `import/export` module)
- AST-derived declaration counters (value/type/top-level totals)
- visibility/control-flow capability flags
- early semantic validation for:
  - top-level-only `export` / `private`
  - duplicate top-level declaration names
  - AST shape invariants (ordered, non-overlapping node spans)

Lowering currently preserves behavior by delegating to the legacy compiler:

- `compileSinglePassLegacy(...)` in `src/frontend/singlepass_parse.c`
- `lowerEmitBytecode(...)` in `src/bytecode/pipeline_lower.c` is the backend
  seam invoked by pipeline orchestration.
- top-level `optimizeChunk(...)` is now owned by lower stage (phase 6 migration),
  using `compileSinglePassLegacyUnoptimized(...)` + explicit optimize call.

## Next refactor targets

- Expand the current top-level AST into full statement/expression AST nodes.
- Move type-resolution and declaration checks from legacy compile into sema.
- Move bytecode emission to a dedicated backend lowering pass.

No language semantics changed by this pipeline step.
