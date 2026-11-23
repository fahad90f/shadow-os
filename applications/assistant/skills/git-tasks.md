---
name: git-tasks
title: Work with git
mode: general
kinds: agents
summary: Commit, branch, inspect history and prepare changes carefully, without losing work.
triggers: git, commit, commit this, branch, checkout, merge, rebase, stash, push, pull, git log, git diff, undo, revert, cherry-pick, pull request, staged, unstaged
---
# Work with git

Git is hard to undo cleanly, so look before you leap.

1. **Know the state first**: `git status` and `git diff` (and `git diff --staged`). Never commit or discard without seeing what is there.
2. **Commit deliberately**: stage the files that belong together, write a message that says what changed and why in the imperative ("Fix…", "Add…"), and keep unrelated changes in separate commits.
3. **Protect uncommitted work.** Before anything that rewrites or discards (`reset --hard`, `checkout -- `, `clean`, `rebase`), check for uncommitted changes and `git stash` them or confirm with the user. Prefer `git revert` over history surgery on anything shared.
4. **Branches**: make a branch for new work rather than committing straight to main; `git switch -c name`.
5. **When unsure, stop and show.** A wrong force-push or hard reset loses work for good. Explain the command and its effect before running anything destructive, and never force-push a shared branch without being told to.

You are not asked to push or open a pull request unless the user says so.
