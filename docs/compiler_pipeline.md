# Compiler pipeline

Erkao routes `compile()` through explicit stage contracts:

1. Frontend stage (`src/frontend/pipeline_frontend.c`)
2. Sema stage (`src/typecheck/pipeline_sema.c`)
3. Lower/codegen stage (`src/bytecode/compiler_pipeline.c`)

## Stage contracts

Frontend now builds a structured `FrontendUnit` from tokens/source:

- validates stream invariants (`TOKEN_ERROR` rejection, final `TOKEN_EOF`)
- captures feature flags (imports, exports, classes, loops, match/switch, etc.)
- captures top-level declaration stats
- captures depth stats (paren/brace/bracket/interpolation max depth)

Sema builds a `SemaUnit` summary from `FrontendUnit`:

- module kind (`script` vs `import/export` module)
- declaration counters (value/type/top-level totals)
- visibility/control-flow capability flags

Lowering currently preserves behavior by delegating to the legacy compiler:

- `compileSinglePassLegacy(...)` in `src/frontend/singlepass_parse.c`

## Next refactor targets

- Replace `FrontendUnit` token summary with a true AST representation.
- Move type-resolution and declaration checks from legacy compile into sema.
- Move bytecode emission to a dedicated backend lowering pass.

No language semantics changed by this pipeline step.
