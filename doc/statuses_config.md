# GitBoard Statuses Configuration

GitBoard workflow statuses are defined in `config/statuses.json`.
The file is loaded when the application starts, and the resulting `STATUSES`
mapping is used by status grouping and transition validation.

## File Shape

The top-level JSON value is an object. Each key is a status identifier used in
task front matter:

```json
{
  "todo": {
    "order": 1,
    "display": "To Do",
    "transitions": ["in_progress", "backlog"]
  }
}
```

Task files store the status key, not the display label:

```yaml
status: todo
```

## Status Keys

Status keys are stable machine-readable identifiers. They are used by:

- task front matter in `tasks/*.md`
- `gitboard move TASK-001 todo`
- TUI status grouping and move prompts
- board column grouping

Use lowercase snake_case keys for consistency with the existing file.

Current status keys are:

- `backlog`
- `todo`
- `in_progress`
- `review`
- `done`
- `released`
- `archived`

## Fields

### `order`

Integer sort position for status columns and TUI status buttons.

Statuses are sorted in ascending `order`. Lower numbers appear earlier.
Every status should have a unique `order` value so ordering is predictable.

Used by:

- `gitboard/clicommands/board.py`
- `gitboard/tui_app.py`

### `display`

Human-readable label for the status.

This label is shown in board columns and TUI status buttons. It does not need
to match the status key.

Used by:

- `gitboard/task_view.py`
- `gitboard/tui_app.py`

### `transitions`

List of status keys that a task may move to from this status.

Example:

```json
"in_progress": {
  "order": 2,
  "display": "In Progress",
  "transitions": []
}
```

`gitboard.statuses.can_transition()` enforces this list when moving tasks.
If the list contains values, the destination status must be one of those
values.

Important behavior: an empty list means unrestricted movement to any status.
It does not mean that a task cannot move. The default status configuration uses
empty transition lists for every status, so default movement is unrestricted.

If a task is already in an undefined status, GitBoard allows it to move out to
any destination status.

Used by:

- status transition validation
- move operations
- future board or TUI grouping

### `visible`

Optional field currently present on `archived`:

```json
"visible": false
```

No current GitBoard command reads this field. If a board or TUI view is added,
it may use this boolean to hide archived tasks from default views.

## Current Workflow

The default status order is:

```text
backlog
todo -> in_progress -> review -> done -> released -> archived
```

This order is used for display and grouping. It does not restrict movement in
the default configuration because every status has an empty `transitions` list.

## Undefined Statuses

A task can contain a status that is not defined in `statuses.json`.

CLI behavior:

- `gitboard move` warns when the destination is unknown, but still moves the
  task if the transition rule allows it.
- `gitboard board` places tasks with unknown statuses in an `Undefined`
  column.

TUI behavior:

- The move prompt lists known statuses, but the entered value is not required
  to be one of them.
- Status buttons are built only from known statuses. Tasks in unknown statuses
  are grouped internally but are not reachable through normal status buttons.

## Adding A Status

To add a new status:

1. Add a new top-level key in `config/statuses.json`.
2. Set a unique `order`.
3. Set the `display` label.
4. Set `transitions` to the allowed destination status keys.
5. Add the new key to other statuses' `transitions` lists where movement into
   the new status should be allowed.
6. Update any task front matter that should use the new status.

Example:

```json
"blocked": {
  "order": 3,
  "display": "Blocked",
  "transitions": ["todo", "in_progress", "backlog"]
}
```

If inserting a status between existing statuses, update surrounding `order`
values so the board and TUI stay in the intended order.

## Validation Notes

The JSON file is not schema-validated. Missing fields or incorrect types can
raise runtime errors in commands that expect them.

Practical requirements:

- Every status should define `order`, `display`, and `transitions`.
- `order` should be an integer.
- `display` should be a string.
- `transitions` should be a list of status-key strings.
- Transition target keys should normally exist in the same file.
