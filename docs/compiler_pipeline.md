# Compiler pipeline

Erkao now routes `compile()` through explicit stages:

1. Frontend stage (`src/frontend/pipeline_frontend.c`)
2. Sema stage (`src/typecheck/pipeline_sema.c`)
3. Lower/codegen stage (`src/bytecode/compiler_pipeline.c`)

Current behavior is preserved by delegating stage 3 to the legacy single-pass
compiler implementation:

- `compileSinglePassLegacy(...)` in `src/frontend/singlepass_parse.c`

This establishes stable seams for the next refactor steps:

- Replace frontend stage payload with a real AST.
- Move type analysis from legacy compile into sema stage.
- Move bytecode emission into dedicated backend lowering stage.

No language semantics changed in this step.
