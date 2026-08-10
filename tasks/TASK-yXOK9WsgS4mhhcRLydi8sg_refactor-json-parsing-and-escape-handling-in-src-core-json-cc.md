---
assignee: ''
branches: []
ci_status: unknown
created_at: 2026-07-07T16:11:46.677232Z
created_by: ''
id: TASK-yXOK9WsgS4mhhcRLydi8sg
priority: medium
prs: []
status: backlog
story_points: 5
tags: []
title: 'Refactor JSON parsing and escape handling in src/core/json.cc'
updated_at: 2026-07-07T16:11:46.763495Z
---

### Code Review Findings
- **Lines 147-149**: After detecting a decimal point, the parser advances `pos_` but does not verify that at least one digit follows; this can lead to an empty fraction and undefined behavior.
- **Lines 185-190**: The escape handling in `parse_string` uses a `switch` on the escaped character but does not validate that the escaped sequence is complete (e.g., missing closing backslash), leaving potential buffer over‑reads.
- **Lines 22-36**: `append_utf8` implements UTF‑8 encoding via nested `if‑else`; a lookup table or a `switch` would improve readability and performance.
- **Lines 42-44**: `high_surrogate` and `low_surrogate` are simple predicate functions; marking them `constexpr` and possibly inlining them would allow compile‑time evaluation.
- **Lines 61-95**: The recursive `parse_value` increments depth before checking against `kMaxJsonDepth` in the next call; moving the depth check earlier would provide clearer error messages for overly deep JSON.
- **Exception safety**: `throw error(...)` is scattered throughout the parser; centralising error creation could improve consistency and allow future enhancements (e.g., richer error codes).
- **Performance**: Repeated string concatenations in `parse_string` may be costly for long strings; reserving capacity based on an estimate could reduce reallocations.
- **Testing coverage**: No tests for edge cases such as surrogate pairs, empty strings, or JSON depth limits.

### Recommendations
1. Add a check after consuming `'.'` to ensure at least one digit follows before continuing.
2. Replace the `switch` in `parse_string` with a table‑driven decoder that validates complete escape sequences and rejects unknown ones.
3. Mark simple predicate functions (`high_surrogate`, `low_surrogate`) as `constexpr` and consider merging them into a single utility.
4. Refactor depth validation to occur immediately before recursive calls, with a clear error message.
5. Optimise `parse_string` by pre‑allocating sufficient capacity and using a fast escape decode.
6. Introduce unit tests covering surrogate handling, malformed escapes, and depth overflow scenarios.
7. Centralise error creation to allow richer error types and future extensibility.

Estimated effort: 5 story points.
