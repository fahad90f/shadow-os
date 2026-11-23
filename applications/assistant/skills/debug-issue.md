---
name: debug-issue
title: Find a bug
mode: general
kinds: coding
summary: Reproduce a crash, failing test or wrong result, narrow it to one cause, then fix the cause.
triggers: debug, crash, crashes, segfault, stack trace, traceback, exception, error, not working, broken, fails, failing, failing test, wrong output, doesn't work, why does, fix the, won't build, build error, compile error, hangs, freezes
---
# Find a bug

Work from evidence, not guesses. A fix you cannot explain is a guess.

1. **Reproduce it.** Get the exact command, input and error. Run it yourself and see the failure before changing anything. If you cannot reproduce it, say what you need to.
2. **Read the real error.** The first error, not the last; the top frame in your code, not the library's. A compile error means the first message, the others are usually its echoes.
3. **Narrow it.** Halve the space: comment out, add a print or a breakpoint, check the inputs at the boundary, `git bisect` or `git log` the file if it worked before. State what you ruled out.
4. **Name the cause** in one sentence before you touch the code: "it crashes because X is empty when Y runs." If you cannot, you have not found it yet.
5. **Fix the cause, not the symptom.** Then reproduce again and watch the same case pass. Run the project's tests for what you touched. If it was a real bug, consider a test that would have caught it.

Do not scatter changes hoping one works, and do not silence a warning or a failing check to make the red go away.
