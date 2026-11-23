---
name: code-review
title: Review a change
mode: general
kinds: coding
summary: Read a diff or file and report real bugs, security holes and unclear code, worst first.
triggers: review, code review, review this, look over, check my code, is this correct, diff, pull request, pr, merge request, lgtm, find bugs, bug in, what's wrong with, any issues
---
# Review a change

Review code the way a careful engineer does: find defects that would actually bite, and say so plainly.

1. **See the change.** If it is a file, read it. If it is a repository change, run `git diff` (or `git diff --staged`), and read the surrounding code of anything you are unsure about — a diff alone hides the context a bug needs.
2. **Judge correctness first.** For each change ask: what input or state makes this do the wrong thing? Off-by-one, null/empty, wrong sign, an await or lock missing, a resource not freed, an error swallowed, a case not handled. Only report a defect you can tie to a concrete failing case.
3. **Then security.** Untrusted input reaching a shell, SQL, a path, or HTML; a secret written to a log or committed; a check that can be skipped; weak randomness where it matters.
4. **Then clarity and waste**, but briefly: a name that misleads, duplicated logic, a simpler form. Do not drown the real bugs in style notes.
5. **Report**, worst first. For each: the file and line, one sentence on the defect, the input or state that triggers it, and the smallest fix. If you found nothing serious, say so rather than inventing concerns.

Do not rewrite the whole thing. Point at what is wrong and let the user decide. Never weaken or delete a test to make something pass.
