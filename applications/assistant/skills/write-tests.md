---
name: write-tests
title: Write tests
mode: general
kinds: coding
summary: Add tests that would fail without the code under test and pass with it, then run them.
triggers: test, tests, write tests, unit test, add a test, test coverage, test this, pytest, jest, junit, test case, write a test, cover this, testing
---
# Write tests

A test earns its place by failing when the code is wrong.

1. **Find how this project tests.** Look for its runner and existing tests (pytest, jest, go test, ctest, a `tests/` folder, a `Makefile` target) and follow their style, imports and naming. Do not introduce a new framework unasked.
2. **Cover behaviour, not lines.** The normal case, the boundaries (empty, zero, one, max, negative), the error paths, and any bug this change fixes (a test that fails on the old code).
3. **Make each test independent and deterministic** — no order dependence, no real network or clock unless that is the point; use the project's fixtures or mocks.
4. **Run them.** Watch the new tests pass and the existing ones stay green. A test you did not run is not done.
5. **Prove the test bites:** where practical, break the code for a moment and confirm the test goes red, so you know it tests something.

Report the command to run them and what each one checks. Never make a test pass by weakening it.
