---
name: explain-code
title: Explain code
mode: general
kinds: coding
summary: Explain what a file, function or codebase does, from reading it, at the depth asked.
triggers: explain, what does this do, how does this work, understand, walk me through, what is this, read this code, trace, where is, how is, architecture, overview of
---
# Explain code

Explain from what the code actually does, not from its names.

1. **Read it**, and the things it calls that matter. For a whole codebase, start from the entry point (main, the server's routes, the CLI) and the build file, then follow the few paths that carry the real work. Search for a symbol rather than guessing where it lives.
2. **Say what it does before how.** One or two sentences on its job and where it sits, then the flow: inputs → the steps that matter → outputs and side effects (files, network, state).
3. **Point at the code**, by `file:line`, so the user can follow. Quote the key lines, not whole blocks.
4. **Name the sharp edges**: the non-obvious assumption, the global state, the thing that looks wrong but is deliberate, the part you are unsure of — and say you are unsure rather than smoothing it over.

Match the depth to the question: a one-line "what is this" gets a short answer; "walk me through it" gets the flow. Do not pad.
