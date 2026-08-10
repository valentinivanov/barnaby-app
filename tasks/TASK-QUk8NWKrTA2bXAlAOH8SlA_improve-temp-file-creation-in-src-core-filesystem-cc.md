---
assignee: cody
branches: []
ci_status: unknown
created_at: 2026-07-07T15:42:52.855095Z
created_by: ''
id: TASK-QUk8NWKrTA2bXAlAOH8SlA
priority: medium
prs: []
status: backlog
story_points: 5
tags:
- filesystem
- security
title: 'Improve temp file creation in src/core/filesystem.cc'
updated_at: 2026-08-07T15:32:28.058213Z
---

The current implementation of create_temp_path_for uses predictable naming and platform-specific APIs that can lead to race conditions and security issues. Refactor to use std::filesystem::temp_directory_path and std::filesystem::unique_path (C++17) to generate secure temporary files, add explicit error handling, and ensure atomic replacement semantics are preserved. Add unit tests covering both Windows and Linux paths.
