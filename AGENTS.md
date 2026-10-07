# Repository Agent Instructions

These instructions apply to every human or automated agent working in this repository.

## Required change documentation

For every feature, fix, refactor, build-system change, or documentation change:

1. Update `CHANGELOG.md` under `Unreleased` with a concise user-visible description.
2. Update `EVIDENCE.md` with reproducible evidence for the work.
3. Evidence must name the command or procedure, its result, and the feature or acceptance criterion it supports.
4. Never claim a test passed without running it. Clearly label hardware-only checks as pending until observed on a device.
5. Record failures and important limitations as well as successes.
6. Keep roadmap milestone status and acceptance criteria accurate; do not mark a milestone complete based only on compilation when hardware behavior is required.

## Completion checklist

Before proposing a commit:

- Build or run the narrowest relevant validation.
- Run `git diff --check`.
- Update `CHANGELOG.md`.
- Update `EVIDENCE.md`.
- State which hardware checks remain pending.
