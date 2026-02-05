# Refactor Baseline (2026-02-05)

This file freezes the starting point for the large refactor.

## Behavior baseline

- Golden snapshots: `tests/*.out`
- Verification command:
  - `./scripts/run-tests.ps1` (Windows)
  - `./scripts/run-tests.sh` (Unix)
- Current status at freeze time: `55/55` snapshot tests passing.

Architecture baseline:

- Contract: `docs/architecture_contract.json`
- ADR: `docs/adr/0001-layered-architecture-contract.md`
- Current allowed violations: `docs/architecture_violations.json`
- Verification command:
  - `python ./scripts/check-architecture.py`

## Performance baseline

- Baseline file: `bench/baseline.json`
- Capture/update command:
  - `python ./scripts/check-bench.py --update-baseline --repeat 5`
- Gate command:
  - `python ./scripts/check-bench.py --repeat 5 --max-regression-pct 8 --min-slack-ms 20`

Frozen medians (`ms`):

- `arith`: `2185.0`
- `array`: `46.0`
- `map`: `30.0`
- `string`: `21.0`
