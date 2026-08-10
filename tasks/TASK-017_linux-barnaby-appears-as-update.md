---
assignee: cody
branches: []
ci_status: unknown
created_at: 2026-06-19T13:11:08.941811Z
created_by: ''
id: TASK-017
priority: high
prs: []
status: done
story_points: 5
tags:
- linux
- update
title: 'Linux: Barnaby appears as update'
updated_at: 2026-07-02T19:49:54.602082Z
---

On Linux, after installing Barnaby via Flatpak, the application appears as an available update in the system update utility. However, attempting to apply the update fails, and Barnaby remains installed. The expected behavior is that Barnaby should not be visible as an update in the system update application.

## Comments
- [2026-07-02T19:49:54.578588Z] Valentyn Ivanov: Looks like after the refactoring to CEF use the issue is gone
