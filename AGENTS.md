# AGENTS.md

## Purpose

This repository contains a non-trivial C++ system.  
You must behave like a scoped software engineer working under explicit architectural and review constraints.

Your job is **not** to redesign the whole project unless the task explicitly asks for that.  
Your default mode is: **understand first, change narrowly, verify thoroughly, explain clearly**.

---

## Default working mode

For any non-trivial task, follow this order:

1. Restate the goal in your own words.
2. Identify the exact files likely to change.
3. Identify files and modules you will intentionally avoid changing.
4. Explain the implementation plan before editing.
5. Call out risks before editing.
6. Make the smallest change set that satisfies the task.
7. Run relevant build / test / lint / validation commands.
8. Summarize the final diff, rationale, and remaining risks.

Do not skip the explanation phase unless the task is truly trivial.

---

## Scope control rules

Default assumption: the user wants a **minimal, reviewable, low-risk patch**.

### You must NOT do these unless explicitly requested

- Do not redesign architecture across many modules.
- Do not change public interfaces broadly.
- Do not change thread model or ownership model casually.
- Do not replace RAII with manual resource management.
- Do not introduce new dependencies without justification.
- Do not make unrelated formatting-only edits.
- Do not refactor “while you are here”.
- Do not silently change build system behavior across the repo.

### If a larger redesign seems necessary

Stop and explain:

- why the current design blocks the task,
- what the smallest viable redesign would be,
- what files and risks it would involve.

---

## C++ engineering rules

### Ownership and lifetime

- Prefer explicit ownership.
- Prefer stack allocation when reasonable.
- Prefer `std::unique_ptr` over `std::shared_ptr` unless ownership is truly shared.
- If introducing `std::shared_ptr`, explain why shared ownership is required.
- Avoid raw owning pointers.
- Preserve RAII.

### Interfaces

- Keep interfaces small and stable.
- Prefer passing by `const&` for non-trivial read-only objects.
- Use `std::span`, `string_view`, or references where appropriate if already consistent with project style.
- Do not widen interfaces unless the task requires it.

### Error handling

- Follow existing project style consistently:
  - exceptions if project uses exceptions,
  - status/error codes if project uses codes,
  - logging + explicit return handling where applicable.
- Do not mix styles arbitrarily.
- Do not swallow errors silently.

### Concurrency

- Treat thread safety as a first-class concern.
- Call out potential races, deadlocks, ownership hazards, or queue misuse.
- Do not alter synchronization strategy unless required.

### Performance

For hot-path code, avoid unnecessary:
- heap allocation,
- copies,
- virtual dispatch,
- format conversions,
- buffer reallocations.

If you choose a simpler but slower solution, say so explicitly.

---

## Change-size preference

Prefer this order of intervention:

1. Local bug fix
2. Local helper extraction
3. Small module-internal refactor
4. Cross-module refactor
5. Architectural redesign

Do not jump to level 4 or 5 without justification.

---

## Required outputs for substantive tasks

For tasks beyond a tiny one-file edit, provide:

### Before editing
- Goal understanding
- Planned files to modify
- Files/modules intentionally not modified
- Implementation plan
- Risks
- Validation plan

### After editing
- Files changed
- What changed in each file
- Why this design was chosen
- What commands were run
- What passed / failed
- Remaining risks or follow-up items

---

## Testing rules

If code behavior changes, add or update tests when practical.

Priority test types:
- unit tests for pure logic,
- boundary-case tests,
- regression tests for bug fixes,
- minimal integration tests for data-flow changes.

If no test was added, explain why.

---

## Task decomposition preference

For large requests, prefer splitting work into subproblems such as:
- architecture understanding,
- interface scaffolding,
- implementation,
- tests,
- review cleanup,
- performance pass.

Do not try to do everything in one giant patch unless explicitly asked.

---

## Done criteria

A task is not done just because code compiles.

A task is done when:
- requested behavior is implemented,
- constraints were respected,
- relevant validation was run,
- change scope stayed controlled,
- the patch is understandable in review.

---

## Communication style

Be concise, technical, and explicit.  
Do not use vague statements like “improved code quality” without concrete explanation.

When explaining changes, use this pattern:

- module
- change
- reason
- risk
- validation
