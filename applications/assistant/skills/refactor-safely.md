---
name: refactor-safely
title: Refactor safely
mode: general
kinds: coding
summary: Restructure code without changing what it does, in small checked steps.
triggers: refactor, clean up, tidy, restructure, rename, extract, simplify, split up, deduplicate, reorganize, make this cleaner, improve this code
---
# Refactor safely

Refactoring changes the shape, never the behaviour. If behaviour changes, it is a rewrite — say so and get agreement first.

1. **Pin the behaviour.** Make sure there are tests for what you are about to move; if there are none, add a couple first, or at least capture the current output to compare against.
2. **One small step at a time**: rename, extract a function, inline, move. After each step, build and run the tests. Many small verified steps beat one big leap.
3. **Keep the public surface** (signatures, exports, files others import) unless the task is to change it; when you must, update every caller in the same change and search to be sure you found them all.
4. **Do not smuggle in fixes or features.** If you spot a bug while refactoring, note it separately; mixing the two makes both impossible to review.
5. **End green**: the same tests pass, the diff is only structure, and you can say in one line why it is clearer.
