# Context

Recent refactor work touched critical runtime modules (`exec`, `imports`, `stdlib_http`) while adding stronger security and quality gates.

# Decision

1. Keep unsafe runtime capabilities default-off and add explicit CLI runtime policy:
   - `--allow-unsafe=none|proc|ffi|plugins|all`
   - Explicit CLI policy overrides env toggles when provided.
2. Add CI-enforced code-size gate (`scripts/check-code-size.py`) with zero-new-issues baseline.
3. Add CI design-note gate for non-trivial critical-module changes (`scripts/check-design-note.py`).
4. Add security tests for:
   - package export path traversal escape
   - unsafe policy bypass attempts
   - command-shell injection style misuse in `proc.run`
5. Add fuzz targets for `imports` and HTTP parser internals.

# Alternatives Considered

- Keep env-only unsafe toggles: rejected due weak auditability and policy bypass risk.
- Enforce hard size limits without baseline: rejected because existing debt would block all PRs.
- Add design-note checks manually in review only: rejected because it drifts over time.

# Risks And Mitigations

- Risk: CI noise from governance checks.
  - Mitigation: baseline/threshold approach and explicit critical-file scope.
- Risk: behavior change for unsafe feature enablement.
  - Mitigation: compatibility kept through env toggles when explicit policy is not set.

# Test and Perf Impact

- Added tests: `63_ffi_allow_unsafe_flag`, `64_plugin_policy_bypass_blocked`,
  `65_import_package_escape`, `66_proc_shell_rejected`.
- Existing test suite remains passing.
- CI gains additional low-cost governance checks and fuzz surface.
