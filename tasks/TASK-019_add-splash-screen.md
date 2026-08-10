---
assignee: cody
branches: []
ci_status: unknown
created_at: 2026-06-30T14:22:23.967669Z
created_by: ''
id: TASK-019
priority: medium
prs: []
status: done
story_points: 5
tags: []
title: 'Add Splash Screen'
updated_at: 2026-07-02T19:19:12.670992Z
---

# Summary

- Add a splash screen that appears immediately after the app is launched.

## Description

- The splash screen should display the Barnaby icon and the text "Barnaby".
- It should be shown while the application performs its startup tasks.
- The splash screen must disappear as soon as the main window is presented.
- it should work in the same manner on all target platforms - macOS, linux, windows.

## Acceptance Criteria

- [] Splash screen appears on app launch.
- [] Icon and text are displayed correctly.
- [] Splash screen automatically dismisses once the main window is visible.
- [] No flickering or leftover UI elements.

## Subtasks

- [] Design splash screen visual assets.
- [] Implement splash screen logic.
- [] Add dismissal trigger after main window shows.
- [] Update documentation and add inline comments.

## Documentation

- [] Update relevant docs with splash screen instructions.
- [] Add inline comments in code.

## Tests

- [] Code coverage for splash screen code.
- [] Unit tests for splash screen behavior.
- [] Integration tests ensuring splash disappears at the right time.

## Dependencies

- [] None identified.
- [] May depend on existing icon assets.
