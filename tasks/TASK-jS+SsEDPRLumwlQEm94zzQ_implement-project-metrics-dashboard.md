---
assignee: cody
branches: []
ci_status: unknown
created_at: 2026-08-05T12:45:37.755154Z
created_by: ''
id: TASK-jS+SsEDPRLumwlQEm94zzQ
priority: medium
prs: []
status: todo
story_points: 13
tags:
- metrics
- dashboard
- analytics
title: 'Implement Project Metrics Dashboard'
updated_at: 2026-08-07T15:32:09.305422Z
---

## Description

Create a dashboard that surfaces key project metrics for better visibility and planning.

### Suggested Metrics & Implementation Guidance

1. **Status Distribution** – Count of tasks per workflow status (Backlog, To Do, In Progress, Review, Done, Archived). Shows bottlenecks.

2. **Story‑Point Velocity** – Aggregate completed story points per sprint/iteration. Use to forecast velocity and capacity.

3. **Work‑In‑Progress (WIP) Limits** – Number of tasks currently In Progress or Review. Enforce explicit WIP limits to reduce multitasking.

4. **Cycle Time & Lead Time** – Average time from status entry (e.g., To Do) to Done, and from request to start. Helps predict delivery dates.

5. **Task Age / Aging** – Duration a task has spent in each status. Highlights stale or abandoned items.

6. **Priority Distribution** – Count of High/Medium/Low priority tasks. Ensures focus on urgent work without backlog overload.

7. **Assignee Workload** – Story points or task count per team member. Use to balance load and detect overallocation.

8. **Tag / Theme Coverage** – Distribution of tasks by tag (e.g., feature, bug, documentation). Verify all required areas are addressed.

9. **Completed vs. Planned vs. Remaining** – Compare planned story points vs. completed vs. remaining. Drives stakeholder updates.

10. **Blocked / Dependency Tasks** – Identify tasks that are blocked or have unmet dependencies. Trigger mitigation steps.

11. **Automation / Tech‑Debt Ratio** – Count of tasks focused on maintenance (e.g., git integration, refactors) versus new feature work. Balance technical health.

### Data Model Decision

Use explicit task-file history trails for metrics that depend on changes over
time. Git history may be useful later as an audit or one-time import source, but
dashboard metrics should not depend on inferring field transitions from commits.
Commit history is sensitive to squashes, rebases, partial commits, unpublished
work, merge strategy, and commit timestamps, so it is too workflow-dependent to
be the primary metrics model.

Keep existing scalar fields as the authoritative current state, and add
append-only history arrays for fields where historical transitions matter. The
current scalar keeps old queries, editing, and Markdown readability simple; the
history trail enables velocity, cycle time, lead time, and time-in-status
calculations.

Fields needed:

- `status_history`: array of status transition objects.
  - `at`: UTC timestamp for the transition.
  - `from`: previous status, empty for the initial recorded status.
  - `to`: new status.
  - `by`: user/actor that made the transition.
- `sprint`: optional scalar sprint/iteration id currently assigned to the task.
- `sprint_history`: optional array of sprint assignment changes.
  - `at`: UTC timestamp for the change.
  - `from`: previous sprint id, empty if none.
  - `to`: new sprint id, empty if removed.
  - `by`: user/actor that made the change.
- `estimate_history`: optional array of story-point changes.
  - `at`: UTC timestamp for the change.
  - `from`: previous story-point value.
  - `to`: new story-point value.
  - `by`: user/actor that made the change.

The first implementation should prioritize `status_history` and `sprint`, since
those are required for accurate cycle time, lead time, time spent in each status,
and sprint velocity. `sprint_history` and `estimate_history` can follow if the
dashboard needs planned-vs-completed reporting that reflects mid-sprint scope or
estimate changes.

For existing tasks without history, synthesize a partial baseline from
`created_at` and the current `status`, mark derived timing metrics as partial,
and begin writing real history on the next status or sprint update.

### Suggested Implementation Approach

- Add a new JSON endpoint `/metrics` that returns aggregated counts and sums based on task fields.
- Extend the task model with optional timestamps for status transitions (if not existing) to compute cycle/lead times.
- Create a simple UI component (e.g., a side panel or dedicated page) that visualizes the above metrics using a chart library.
- Add unit tests covering each metric calculation and edge cases (e.g., tasks with no timestamps).
- Document the metrics in the project wiki and add onboarding notes for new contributors.

---
*All suggestions are aimed at providing a clear, actionable view of project health and progress.*
