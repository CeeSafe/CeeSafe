# ADR 0185 — `ceesafec test --show-output`

- Status: **Accepted** by the owner, 2026-11-20 (asked whether to accept the designs proposed under "fix all findings": "Accept all four"); proposed 2026-11-19, done under the owner's instruction to fix every finding of CeeUsb ("Now we focus on CeeSafe and fix all findings"). Implemented 2026-11-19; released
  in 0.79.0.
- Scope: tooling.md §10.2. Finding F10.

## 1. Context

`ceesafec test` shows a test's captured output only when it fails. CeeUsb's hardware tests print what they found
(the devices, the bytes a transfer moved) and pass; to see it, the test had to be run as a program.

## 2. Decision

`--show-output` shows the captured standard output of tests that passed too, under their names, as a failed test's
is shown. Without it nothing changes. `--format json` already has every test's output and is unchanged.
`ceesafec test --help` lists it.

## 3. Implementation and tests (2026-11-19)

- `Options.testShowOutput` (src/main.cee), the report (src/host/test_runner.cee).
- `tests/packages/test-command` gained `printsAndPasses` (expected.tsv, with a review note); `tools/test_command_check.py`
  check 2b runs with and without the option and checks the help. 0 problems.
