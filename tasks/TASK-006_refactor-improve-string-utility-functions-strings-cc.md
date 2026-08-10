---
assignee: valentyn
branches: []
ci_status: unknown
created_at: 2026-06-10T21:59:13.127544Z
created_by: ''
id: TASK-006
priority: low
prs: []
status: backlog
story_points: 2
tags: []
title: 'Refactor & Improve String Utility Functions (strings.cc)'
updated_at: 2026-07-01T17:47:15.214644Z
---

Review `src/core/strings.cc` for performance improvements, robustness, and adherence to C++ best practices.

**Key areas for refinement:**
*   **JSON Escaping:** Overhaul `json_escape` to guarantee proper escaping of all control characters across the entire ASCII range, not just specific escapes or low-ASCII ranges. Treat non-printable bytes safely.
*   **Efficiency in String Splitting:** Consider if leveraging `std::stringstream` in `split()` can be replaced by a manual search algorithm for performance optimization if the strings processed are large.
*   **General Review:** Tweak helper functions (e.g., making character manipulations more idiomatic using C++ algorithms) while maintaining current functionality and correctness.
