---
assignee: cody
branches: []
ci_status: unknown
created_at: 2026-06-30T17:44:14.888525Z
created_by: ''
id: TASK-020
priority: high
prs: []
status: done
story_points: 3
tags: []
title: 'Stable task id generation'
updated_at: 2026-06-30T20:30:06Z
---

# Task title
Task id should be generated in a independently unique basis and not based on last id from the curent version of the repo.

## Description
Currently task id is generated based on a local repo state. It’s posible to run into collision when creating task based on the same state of the repo on different machines.
Need to propose a scheme when task ids would be globally unique.
