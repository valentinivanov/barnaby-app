---
assignee: cody
branches: []
ci_status: unknown
created_at: 2026-06-17T12:58:15.810707Z
created_by: ''
id: TASK-016
priority: high
prs: []
status: done
story_points: 5
tags: []
title: 'Extend task properties with story points'
updated_at: 2026-06-18T10:38:06.064198Z
---

# Summary

- 

## Description
Implement a new `story_points` field for tasks. The field must be a numerical value drawn from the set {1, 2, 3, 5, 8, 13, 21, 100}. By default, newly created tasks should receive a story point value of 100. In the UI, users should select the story point value via a dropdown list.
There should be a ‘gitboard points TASK-ID NEW_VALUE’ command allowing to change points for the given task. Make sure the server can use the new property, also add proper tools for Agent Pip so he can manipulate the story points value.

## Acceptance Criteria
- [ ] The `story_points` field is added to the task schema.
- [ ] Allowed values are restricted to 1, 2, 3, 5, 8, 13, 21, 100.
- [ ] Default value for new tasks is 100.
- [ ] UI component (dropdown) is integrated and persists the selected value.
- [ ] Existing tasks are assumed to have value 100 for the story points and should be updated after saving.
-

## Documentation
- Update relevant documentation with the new field description.
- Add inline comments explaining validation logic.

